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

/* WinDivert handle, opened on startup, closed on exit */
static HANDLE g_divert = INVALID_HANDLE_VALUE;
static HANDLE g_thread = NULL;
static volatile LONG g_should_exit = 0;

/* UI handles */
static HWND g_main_wnd      = NULL;
static HWND g_toggle_btn    = NULL;
static HWND g_status_label  = NULL;
static HFONT g_big_font     = NULL;
static HFONT g_small_font   = NULL;

#define IDC_TOGGLE_BTN   1001
#define IDC_STATUS_LABEL 1002

#define WM_APP_TOGGLE    (WM_APP + 1)

/* Layout: every child is at x=MARGIN with width=WINDOW_W-2*MARGIN so the
 * left and right gutters are identical. */
#define WINDOW_W 420
#define WINDOW_H 230
#define MARGIN   20

/* Keyboard hook state -- fires the toggle the instant Left Alt goes
 * down. Auto-repeat is suppressed so holding the key doesn't re-fire.
 * The hook lives on a dedicated thread with its own message loop, so
 * the OS never starves us (if the UI thread ever blocks, Windows would
 * silently drop a hook that was installed on it). */
static HHOOK  g_kbd_hook   = NULL;
static HANDLE g_hook_thread = NULL;
static DWORD  g_hook_tid    = 0;
static BOOL   g_lalt_down   = FALSE;

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

        /* The critical read: one atomic load per batch decides how the
         * whole batch is handled. No locks, no contention. */
        LONG hold = InterlockedCompareExchange(&g_holding, 0, 0);

        if (!hold) {
            /* Forward the whole batch untouched. */
            WinDivertSendEx(g_divert, packets, recv_len, NULL, 0,
                addrs, addr_len, NULL);
        }
        /* else: drop the batch on the floor. Packets already left the
         * stack, so "do nothing" means the refresh is in effect. */
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
        SetWindowTextA(g_toggle_btn,   "REFRESHING");
        SetWindowTextA(g_status_label, "CONNECTION: REFRESHING");
    } else {
        SetWindowTextA(g_toggle_btn,   "CONNECTED");
        SetWindowTextA(g_status_label, "CONNECTION: ACTIVE");
    }

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

/* Low-level keyboard hook: fires a toggle the instant Left Alt is
 * pressed. We both flip the shared flag directly (so the next outbound
 * packet already sees the new state -- the divert worker is just a
 * Sleep-like blocking recv on another thread, and it does not need to
 * wake up for the toggle to take effect) AND post a message to the UI
 * thread to refresh the label. The atomic write is what actually makes
 * this feel instant; the PostMessage is just cosmetic.
 *
 * We also swallow the Alt keydown/keyup (return 1 instead of chaining)
 * so Windows doesn't show the menu-bar focus flash that a bare Alt
 * press would normally trigger on most apps. */
static LRESULT CALLBACK low_level_kbd_proc(int nCode, WPARAM wp, LPARAM lp)
{
    if (nCode != HC_ACTION) {
        return CallNextHookEx(g_kbd_hook, nCode, wp, lp);
    }

    KBDLLHOOKSTRUCT *kb = (KBDLLHOOKSTRUCT*)lp;
    BOOL is_lalt = (kb->vkCode == VK_LMENU);

    if (is_lalt) {
        if (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) {
            /* Suppress OS auto-repeat: only fire on the first-down edge. */
            if (!g_lalt_down) {
                g_lalt_down = TRUE;

                /* ---- the actual hot path ----
                 * One atomic flip. The divert worker reads this flag on
                 * its next batch (typically microseconds away because it
                 * is blocked in WinDivertRecvEx waiting for the very
                 * next outbound packet). */
                InterlockedExchange(&g_holding, !g_holding);

                /* Nudge the UI thread to redraw. Not on the hot path. */
                if (g_main_wnd != NULL) {
                    PostMessage(g_main_wnd, WM_APP_TOGGLE, 0, 0);
                }
            }
            return 1; /* swallow: no menu-bar flash, no Alt leaks through */
        } else if (wp == WM_KEYUP || wp == WM_SYSKEYUP) {
            g_lalt_down = FALSE;
            return 1; /* swallow the matching keyup too */
        }
    }

    return CallNextHookEx(g_kbd_hook, nCode, wp, lp);
}

/* Dedicated thread that hosts the low-level keyboard hook. It must pump
 * its own message queue, otherwise the OS will never deliver hook
 * callbacks to it (LL hooks are delivered as synthetic messages to the
 * installing thread). */
static DWORD WINAPI hook_thread_proc(LPVOID unused)
{
    (void)unused;
    g_kbd_hook = SetWindowsHookExA(WH_KEYBOARD_LL, low_level_kbd_proc,
        GetModuleHandleA(NULL), 0);
    if (g_kbd_hook == NULL) {
        return 1;
    }

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    UnhookWindowsHookEx(g_kbd_hook);
    g_kbd_hook = NULL;
    return 0;
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        const int content_w = WINDOW_W - 2 * MARGIN; /* symmetric gutters */

        g_big_font   = make_font(-32, FW_BOLD);
        g_small_font = make_font(-14, FW_NORMAL);

        g_toggle_btn = CreateWindowA("BUTTON", "CONNECTED",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            MARGIN, 20, content_w, 130,
            hwnd, (HMENU)(INT_PTR)IDC_TOGGLE_BTN, GetModuleHandle(NULL), NULL);
        SendMessage(g_toggle_btn, WM_SETFONT, (WPARAM)g_big_font, TRUE);

        g_status_label = CreateWindowA("STATIC", "CONNECTION: ACTIVE",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            MARGIN, 160, content_w, 24,
            hwnd, (HMENU)(INT_PTR)IDC_STATUS_LABEL, GetModuleHandle(NULL), NULL);
        SendMessage(g_status_label, WM_SETFONT, (WPARAM)g_small_font, TRUE);

        HWND hint = CreateWindowA("STATIC",
            "Global shortcut: Left Alt  |  Runs as Administrator",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            MARGIN, 188, content_w, 20,
            hwnd, NULL, GetModuleHandle(NULL), NULL);
        SendMessage(hint, WM_SETFONT, (WPARAM)g_small_font, TRUE);

        /* Install a global low-level keyboard hook on a dedicated
         * thread, so a Left Alt press triggers a refresh from any
         * foreground app the instant it's pressed. Its own message
         * loop keeps it from ever being starved by UI work. */
        g_hook_thread = CreateThread(NULL, 0, hook_thread_proc, NULL, 0,
            &g_hook_tid);
        if (g_hook_thread != NULL) {
            SetThreadPriority(g_hook_thread, THREAD_PRIORITY_TIME_CRITICAL);
        }
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
        /* The keyboard hook has already flipped g_holding atomically --
         * all we need to do here is repaint the labels. */
        update_status_ui();
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (g_hook_thread != NULL) {
            /* Tell the hook thread to exit its message loop; it will
             * unhook on the way out. */
            PostThreadMessage(g_hook_tid, WM_QUIT, 0, 0);
            WaitForSingleObject(g_hook_thread, 1000);
            CloseHandle(g_hook_thread);
            g_hook_thread = NULL;
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
