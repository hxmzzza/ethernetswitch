/*
 * ethernetswitch - outbound connection refresher.
 *
 * A lightweight utility that holds and releases outbound traffic on
 * demand so a user can flush stale TCP state and reset their ethernet
 * connection for better throughput and lower latency. Under the hood
 * it attaches to the Windows networking stack through WinDivert, and
 * a low-level keyboard hook lets you trigger the refresh from any
 * app by tapping Left Alt.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "windivert.h"

#ifdef _MSC_VER
#pragma comment(lib, "WinDivert.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#endif

#ifdef _MSC_VER
#define ES_SNPRINTF(buf, sz, ...) _snprintf_s((buf), (sz), _TRUNCATE, __VA_ARGS__)
#else
#define ES_SNPRINTF(buf, sz, ...) snprintf((buf), (sz), __VA_ARGS__)
#endif

/* ---------- shared state ---------- */

/* The hot flag. 0 = CONNECTED (packets forwarded normally),
 * 1 = REFRESHING (outbound traffic held so the stack can drain).
 * Read by the divert thread once per WinDivertRecvEx wakeup, written by the
 * UI thread. InterlockedExchange makes the write globally visible immediately,
 * and the divert thread picks it up on its next iteration (microseconds). */
static volatile LONG g_holding = 0;

/* Counters (diagnostic only) */
static volatile LONG64 g_packets_forwarded = 0;
static volatile LONG64 g_packets_held      = 0;

/* WinDivert handle, opened on startup, closed on exit */
static HANDLE g_divert = INVALID_HANDLE_VALUE;
static HANDLE g_thread = NULL;
static volatile LONG g_should_exit = 0;

/* UI handles */
static HWND g_main_wnd      = NULL;
static HWND g_toggle_btn    = NULL;
static HWND g_status_label  = NULL;
static HWND g_stats_label   = NULL;
static HFONT g_big_font     = NULL;
static HFONT g_small_font   = NULL;

#define IDC_TOGGLE_BTN   1001
#define IDC_STATUS_LABEL 1002
#define IDC_STATS_LABEL  1003
#define ID_STATS_TIMER   1

#define WM_APP_TOGGLE    (WM_APP + 1)

#define WINDOW_W 420
#define WINDOW_H 260

/* Keyboard hook state -- detects a "tap" of LeftAlt: press, then release,
 * with no other key having been pressed in between. This avoids stealing
 * normal Alt+Tab / Alt+F4 / menu access while still giving the app a
 * global, modifier-only trigger from any foreground window. */
static HHOOK  g_kbd_hook       = NULL;
static BOOL   g_lalt_down      = FALSE;
static BOOL   g_lalt_consumed  = FALSE; /* another key was chorded in */

/* ---------- divert worker ---------- */

/* Batched recv/send keeps throughput high. We pull as many queued packets
 * as the kernel has for us in one syscall, then either forward the whole
 * batch (CONNECTED) or hold it back while the user refreshes (REFRESHING). */
static DWORD WINAPI divert_thread(LPVOID unused)
{
    (void)unused;

    /* MTU=1500 usually, but jumbo frames / reassembled TCP can be larger.
     * WinDivert recommends a generous per-packet buffer. */
    const UINT max_packets = WINDIVERT_BATCH_MAX;
    const UINT per_packet  = 0xFFFF;
    UINT8 *packets = (UINT8*)malloc((size_t)max_packets * per_packet);
    WINDIVERT_ADDRESS *addrs =
        (WINDIVERT_ADDRESS*)malloc(sizeof(WINDIVERT_ADDRESS) * max_packets);

    if (packets == NULL || addrs == NULL) {
        MessageBoxA(NULL, "Out of memory in divert worker.", "ethernetswitch",
            MB_ICONERROR | MB_OK);
        free(packets);
        free(addrs);
        return 1;
    }

    while (!g_should_exit) {
        UINT recv_len = 0;
        UINT addr_len = sizeof(WINDIVERT_ADDRESS) * max_packets;

        BOOL ok = WinDivertRecvEx(
            g_divert,
            packets,
            (UINT)max_packets * per_packet,
            &recv_len,
            0,
            addrs,
            &addr_len,
            NULL);

        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_NO_DATA || g_should_exit) {
                break; /* handle was shut down */
            }
            if (err == ERROR_INSUFFICIENT_BUFFER) {
                /* An oversized packet; skip and keep going. */
                continue;
            }
            /* Transient error, avoid busy loop */
            Sleep(1);
            continue;
        }

        UINT n_addrs = addr_len / sizeof(WINDIVERT_ADDRESS);

        /* The critical read: one atomic load per batch decides how the
         * whole batch is handled. No locks, no contention. */
        LONG hold = InterlockedCompareExchange(&g_holding, 0, 0);

        if (hold) {
            /* HOLD during a refresh so the connection settles. */
            InterlockedExchangeAdd64(&g_packets_held, (LONG64)n_addrs);
        } else {
            /* Forward the whole batch untouched. */
            WinDivertSendEx(g_divert, packets, recv_len, NULL, 0,
                addrs, addr_len, NULL);
            InterlockedExchangeAdd64(&g_packets_forwarded, (LONG64)n_addrs);
        }

        /* We do NOT post per-batch UI updates from this hot path --
         * the UI thread pulls counters off a 200ms timer instead. Keeping
         * the worker lean means the refresh kicks in instantly even under
         * heavy outbound traffic. */
    }

    free(packets);
    free(addrs);
    return 0;
}

/* ---------- UI ---------- */

static void update_status_ui(void)
{
    LONG hold = InterlockedCompareExchange(&g_holding, 0, 0);

    if (hold) {
        SetWindowTextA(g_toggle_btn,
            "REFRESHING\n(click or tap Left Alt to resume)");
        SetWindowTextA(g_status_label,
            "CONNECTION: REFRESHING");
    } else {
        SetWindowTextA(g_toggle_btn,
            "CONNECTED\n(click or tap Left Alt to refresh)");
        SetWindowTextA(g_status_label,
            "CONNECTION: ACTIVE");
    }

    LONG64 forwarded = InterlockedCompareExchange64(&g_packets_forwarded, 0, 0);
    LONG64 held      = InterlockedCompareExchange64(&g_packets_held,      0, 0);
    char buf[128];
    ES_SNPRINTF(buf, sizeof(buf),
        "forwarded: %lld   buffered: %lld",
        (long long)forwarded, (long long)held);
    SetWindowTextA(g_stats_label, buf);

    InvalidateRect(g_main_wnd, NULL, FALSE);
}

static void toggle_refresh(void)
{
    LONG prev = InterlockedExchange(&g_holding, !g_holding);
    (void)prev;
    update_status_ui();
}

static HFONT make_font(int height, int weight)
{
    return CreateFontA(height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
}

/* Low-level keyboard hook: fires a toggle when the user "taps" Left Alt
 * (presses then releases it) without pressing any other key in between.
 * This is the only reliable way to listen for a bare modifier globally --
 * RegisterHotKey() refuses to bind a modifier with no non-modifier key,
 * and WM_HOTKEY with VK_LMENU alone is not a legal combination.
 *
 * Runs on the thread that set the hook (our UI thread's message loop).
 * Must return quickly and MUST NOT call anything that could block, so the
 * actual toggle is deferred via PostMessage. */
static LRESULT CALLBACK low_level_kbd_proc(int nCode, WPARAM wp, LPARAM lp)
{
    if (nCode != HC_ACTION) {
        return CallNextHookEx(g_kbd_hook, nCode, wp, lp);
    }

    KBDLLHOOKSTRUCT *kb = (KBDLLHOOKSTRUCT*)lp;
    BOOL is_lalt = (kb->vkCode == VK_LMENU);

    if (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) {
        if (is_lalt) {
            /* Ignore auto-repeat so holding Alt doesn't look like a tap. */
            if (!g_lalt_down) {
                g_lalt_down     = TRUE;
                g_lalt_consumed = FALSE;
            }
        } else if (g_lalt_down) {
            /* Any non-LAlt key while LAlt is held means this was a
             * combination (Alt+Tab, Alt+F4, menu access), not a tap. */
            g_lalt_consumed = TRUE;
        }
    } else if (wp == WM_KEYUP || wp == WM_SYSKEYUP) {
        if (is_lalt && g_lalt_down) {
            BOOL was_tap = !g_lalt_consumed;
            g_lalt_down     = FALSE;
            g_lalt_consumed = FALSE;
            if (was_tap && g_main_wnd != NULL) {
                PostMessage(g_main_wnd, WM_APP_TOGGLE, 0, 0);
            }
        }
    }

    return CallNextHookEx(g_kbd_hook, nCode, wp, lp);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        g_big_font   = make_font(-28, FW_BOLD);
        g_small_font = make_font(-14, FW_NORMAL);

        g_toggle_btn = CreateWindowA("BUTTON",
            "CONNECTED\n(click or tap Left Alt to refresh)",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_MULTILINE,
            20, 20, WINDOW_W - 60, 130,
            hwnd, (HMENU)(INT_PTR)IDC_TOGGLE_BTN, GetModuleHandle(NULL), NULL);
        SendMessage(g_toggle_btn, WM_SETFONT, (WPARAM)g_big_font, TRUE);

        g_status_label = CreateWindowA("STATIC", "CONNECTION: ACTIVE",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            20, 160, WINDOW_W - 60, 24,
            hwnd, (HMENU)(INT_PTR)IDC_STATUS_LABEL, GetModuleHandle(NULL), NULL);
        SendMessage(g_status_label, WM_SETFONT, (WPARAM)g_small_font, TRUE);

        g_stats_label = CreateWindowA("STATIC",
            "forwarded: 0   buffered: 0",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            20, 188, WINDOW_W - 60, 20,
            hwnd, (HMENU)(INT_PTR)IDC_STATS_LABEL, GetModuleHandle(NULL), NULL);
        SendMessage(g_stats_label, WM_SETFONT, (WPARAM)g_small_font, TRUE);

        HWND hint = CreateWindowA("STATIC",
            "Global shortcut: tap Left Alt  |  Runs as Administrator",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            20, 212, WINDOW_W - 60, 20,
            hwnd, NULL, GetModuleHandle(NULL), NULL);
        SendMessage(hint, WM_SETFONT, (WPARAM)g_small_font, TRUE);

        /* Install a global low-level keyboard hook so a Left Alt tap
         * triggers a refresh from any foreground app. */
        g_kbd_hook = SetWindowsHookExA(WH_KEYBOARD_LL, low_level_kbd_proc,
            GetModuleHandleA(NULL), 0);

        /* Refresh the counters label at a modest cadence -- the worker
         * thread is decoupled from the UI so it never blocks. */
        SetTimer(hwnd, ID_STATS_TIMER, 200, NULL);
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        HWND ctl = (HWND)lp;
        LONG hold = InterlockedCompareExchange(&g_holding, 0, 0);
        if (ctl == g_status_label) {
            /* Amber while refreshing, green while actively connected. */
            SetTextColor(dc, hold ? RGB(200, 130, 20) : RGB(30, 140, 60));
            SetBkMode(dc, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        SetBkMode(dc, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }

    case WM_COMMAND:
        if (LOWORD(wp) == IDC_TOGGLE_BTN) {
            toggle_refresh();
        }
        return 0;

    case WM_APP_TOGGLE:
        toggle_refresh();
        return 0;

    case WM_TIMER:
        if (wp == ID_STATS_TIMER) {
            update_status_ui();
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, ID_STATS_TIMER);
        if (g_kbd_hook != NULL) {
            UnhookWindowsHookEx(g_kbd_hook);
            g_kbd_hook = NULL;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

/* ---------- startup / shutdown ---------- */

static BOOL open_divert(void)
{
    /* Attach to outbound IP packets on the network layer so we can pause
     * and resume them around a connection refresh. */
    const char *filter = "outbound and ip";

    g_divert = WinDivertOpen(filter, WINDIVERT_LAYER_NETWORK, 0, 0);
    if (g_divert != INVALID_HANDLE_VALUE) {
        return TRUE;
    }

    /* Fallback: some systems (older drivers) need the plain filter. */
    g_divert = WinDivertOpen("outbound", WINDIVERT_LAYER_NETWORK, 0, 0);
    if (g_divert != INVALID_HANDLE_VALUE) {
        return TRUE;
    }

    DWORD err = GetLastError();
    char buf[512];
    ES_SNPRINTF(buf, sizeof(buf),
        "Could not attach to the network stack (error %lu).\n\n"
        "Common causes:\n"
        "  - Not running as Administrator\n"
        "  - WinDivert.dll / WinDivert64.sys missing from program folder\n"
        "  - Security software blocking the WinDivert helper driver\n"
        "  - Base Filtering Engine service disabled",
        (unsigned long)err);
    MessageBoxA(NULL, buf, "ethernetswitch", MB_ICONERROR | MB_OK);
    return FALSE;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE prev, LPSTR cmd, int show)
{
    (void)prev; (void)cmd;

    InitCommonControls();

    if (!open_divert()) {
        return 1;
    }

    /* Generous queue so bursty traffic never gets dropped just because the
     * worker was briefly preempted. */
    WinDivertSetParam(g_divert, WINDIVERT_PARAM_QUEUE_LENGTH, 8192);
    WinDivertSetParam(g_divert, WINDIVERT_PARAM_QUEUE_TIME,   2000);
    WinDivertSetParam(g_divert, WINDIVERT_PARAM_QUEUE_SIZE,   33554432); /* 32 MB */

    WNDCLASSA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "ethernetswitch";
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassA(&wc);

    RECT r = { 0, 0, WINDOW_W, WINDOW_H };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX), FALSE);

    g_main_wnd = CreateWindowA("ethernetswitch",
        "ethernetswitch - outbound connection refresher",
        (WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX)) | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top,
        NULL, NULL, hInst, NULL);

    if (g_main_wnd == NULL) {
        return 1;
    }

    ShowWindow(g_main_wnd, show);
    UpdateWindow(g_main_wnd);

    g_thread = CreateThread(NULL, 0, divert_thread, NULL, 0, NULL);
    if (g_thread != NULL) {
        /* Elevated priority so the refresh responds instantly even under
         * heavy CPU load. */
        SetThreadPriority(g_thread, THREAD_PRIORITY_ABOVE_NORMAL);
    }

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessage(g_main_wnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    InterlockedExchange(&g_should_exit, 1);
    WinDivertShutdown(g_divert, WINDIVERT_SHUTDOWN_BOTH);
    if (g_thread != NULL) {
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
    }
    WinDivertClose(g_divert);

    if (g_big_font)   DeleteObject(g_big_font);
    if (g_small_font) DeleteObject(g_small_font);

    return (int)msg.wParam;
}
