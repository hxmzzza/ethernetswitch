/*
 * ethernetswitch - instantly toggle all local outbound packets on/off.
 *
 * A clumsy-style network tool built on WinDivert. The GUI (and an F9
 * global hotkey) flips a single atomic flag; the divert worker reads
 * that flag once per batch of captured packets and either re-injects
 * them (PASS) or drops them (BLOCK). The next outbound packet after a
 * toggle observes the new state, so switching is effectively
 * instantaneous.
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

/* The hot flag. 0 = PASS all outbound (reinject), 1 = BLOCK all outbound (drop).
 * Read by the divert thread once per WinDivertRecvEx wakeup, written by the
 * UI thread. InterlockedExchange makes the write globally visible immediately,
 * and the divert thread picks it up on its next iteration (microseconds). */
static volatile LONG g_blocking = 0;

/* Stats (diagnostic only) */
static volatile LONG64 g_packets_passed  = 0;
static volatile LONG64 g_packets_dropped = 0;

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
#define HOTKEY_ID_TOGGLE 0xC001
#define ID_STATS_TIMER   1

#define WINDOW_W 420
#define WINDOW_H 260

/* ---------- divert worker ---------- */

/* Batched recv/send keeps us close to clumsy's latency profile. We pull
 * as many queued packets as the kernel has for us in one syscall, then
 * either reinject the whole batch (PASS) or discard it (BLOCK). */
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

        /* The critical read: one atomic load, per batch, decides the fate
         * of every packet we just pulled. No locks, no contention. */
        LONG block = InterlockedCompareExchange(&g_blocking, 0, 0);

        if (block) {
            /* DROP: do nothing. Not reinjecting == the packet never leaves
             * the host. Fast path for the "off" state. */
            InterlockedExchangeAdd64(&g_packets_dropped, (LONG64)n_addrs);
        } else {
            /* PASS: reinject the whole batch untouched. */
            WinDivertSendEx(g_divert, packets, recv_len, NULL, 0,
                addrs, addr_len, NULL);
            InterlockedExchangeAdd64(&g_packets_passed, (LONG64)n_addrs);
        }

        /* We do NOT post per-batch UI updates from this hot path --
         * the UI thread pulls stats off a 200ms timer instead. Keeping
         * the worker lean means toggles stay instant even under a
         * 10Gbps outbound torrent. */
    }

    free(packets);
    free(addrs);
    return 0;
}

/* ---------- UI ---------- */

static void update_status_ui(void)
{
    LONG block = InterlockedCompareExchange(&g_blocking, 0, 0);

    if (block) {
        SetWindowTextA(g_toggle_btn, "OFFLINE\n(click or press Space)");
        SetWindowTextA(g_status_label,
            "OUTBOUND PACKETS: BLOCKED");
    } else {
        SetWindowTextA(g_toggle_btn, "ONLINE\n(click or press Space)");
        SetWindowTextA(g_status_label,
            "OUTBOUND PACKETS: PASSING");
    }

    LONG64 passed  = InterlockedCompareExchange64(&g_packets_passed, 0, 0);
    LONG64 dropped = InterlockedCompareExchange64(&g_packets_dropped, 0, 0);
    char buf[128];
    ES_SNPRINTF(buf, sizeof(buf),
        "passed: %lld   dropped: %lld",
        (long long)passed, (long long)dropped);
    SetWindowTextA(g_stats_label, buf);

    InvalidateRect(g_main_wnd, NULL, FALSE);
}

static void toggle_blocking(void)
{
    LONG prev = InterlockedExchange(&g_blocking, !g_blocking);
    (void)prev;
    update_status_ui();
}

static HFONT make_font(int height, int weight)
{
    return CreateFontA(height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        g_big_font   = make_font(-28, FW_BOLD);
        g_small_font = make_font(-14, FW_NORMAL);

        g_toggle_btn = CreateWindowA("BUTTON", "ONLINE\n(click or press Space)",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_MULTILINE,
            20, 20, WINDOW_W - 60, 130,
            hwnd, (HMENU)(INT_PTR)IDC_TOGGLE_BTN, GetModuleHandle(NULL), NULL);
        SendMessage(g_toggle_btn, WM_SETFONT, (WPARAM)g_big_font, TRUE);

        g_status_label = CreateWindowA("STATIC", "OUTBOUND PACKETS: PASSING",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            20, 160, WINDOW_W - 60, 24,
            hwnd, (HMENU)(INT_PTR)IDC_STATUS_LABEL, GetModuleHandle(NULL), NULL);
        SendMessage(g_status_label, WM_SETFONT, (WPARAM)g_small_font, TRUE);

        g_stats_label = CreateWindowA("STATIC",
            "passed: 0   dropped: 0",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            20, 188, WINDOW_W - 60, 20,
            hwnd, (HMENU)(INT_PTR)IDC_STATS_LABEL, GetModuleHandle(NULL), NULL);
        SendMessage(g_stats_label, WM_SETFONT, (WPARAM)g_small_font, TRUE);

        HWND hint = CreateWindowA("STATIC",
            "Global hotkey: F9  |  Requires Administrator",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            20, 212, WINDOW_W - 60, 20,
            hwnd, NULL, GetModuleHandle(NULL), NULL);
        SendMessage(hint, WM_SETFONT, (WPARAM)g_small_font, TRUE);

        /* Register a system-wide hotkey so the app doesn't need focus. */
        RegisterHotKey(hwnd, HOTKEY_ID_TOGGLE, 0, VK_F9);
        /* Refresh the stats label at a modest cadence -- the worker thread
         * is intentionally decoupled from the UI so it never blocks. */
        SetTimer(hwnd, ID_STATS_TIMER, 200, NULL);
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        HWND ctl = (HWND)lp;
        LONG block = InterlockedCompareExchange(&g_blocking, 0, 0);
        if (ctl == g_status_label) {
            SetTextColor(dc, block ? RGB(200, 40, 40) : RGB(30, 140, 60));
            SetBkMode(dc, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        SetBkMode(dc, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }

    case WM_COMMAND:
        if (LOWORD(wp) == IDC_TOGGLE_BTN) {
            toggle_blocking();
        }
        return 0;

    case WM_HOTKEY:
        if (wp == HOTKEY_ID_TOGGLE) {
            toggle_blocking();
        }
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_SPACE) {
            toggle_blocking();
        }
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
        UnregisterHotKey(hwnd, HOTKEY_ID_TOGGLE);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

/* ---------- startup / shutdown ---------- */

static BOOL open_divert(void)
{
    /* Filter: all outbound IP packets (v4 + v6) on the network layer.
     * That covers TCP, UDP, ICMP, and anything else the local stack emits.
     * Loopback packets are considered "outbound" by WinDivert, so a true
     * kill-switch needs them too (optional: && !loopback to leave localhost
     * traffic alone; we block everything for a full clumsy-style switch). */
    const char *filter = "outbound and ip";

    g_divert = WinDivertOpen(filter, WINDIVERT_LAYER_NETWORK, 0, 0);
    if (g_divert != INVALID_HANDLE_VALUE) {
        return TRUE;
    }

    /* Fallback: some systems (older drivers) need an explicit v4/v6 filter */
    g_divert = WinDivertOpen("outbound", WINDIVERT_LAYER_NETWORK, 0, 0);
    if (g_divert != INVALID_HANDLE_VALUE) {
        return TRUE;
    }

    DWORD err = GetLastError();
    char buf[512];
    ES_SNPRINTF(buf, sizeof(buf),
        "WinDivertOpen failed (error %lu).\n\n"
        "Common causes:\n"
        "  - Not running as Administrator\n"
        "  - WinDivert.dll / WinDivert64.sys missing from exe folder\n"
        "  - Antivirus blocking the WinDivert driver\n"
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

    /* Generous queue so bursty traffic during the PASS state doesn't
     * drop packets just because the worker was briefly preempted. */
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
        "ethernetswitch - outbound kill switch",
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
        /* Give the divert worker real-time-ish priority so toggles and
         * reinjection stay responsive under heavy CPU load. */
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
