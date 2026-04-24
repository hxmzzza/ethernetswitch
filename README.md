# ethernetswitch

A small Windows utility that refreshes your outbound ethernet connection
on demand. It briefly pauses outbound traffic so the local TCP stack can
drain stale state, then resumes it cleanly — useful when a long-lived
connection has degraded and you want to reset the link without
disabling the adapter or rebooting.

> Click the big button — or **press Left Alt** from any application —
> to pause outbound traffic. Do it again to resume.

## Running the pre-built binary (no build required)

A ready-to-run build is checked in under [`release/`](./release/). On
Windows:

1. Download the repo (or just the `release/` folder).
2. Double-click `release/run.bat` — it picks the right architecture
   automatically — or drill in to `release/x64/ethernetswitch.exe`
   (use `x86` on 32-bit Windows).
3. Accept the UAC prompt. That's it.

See [`release/README.txt`](./release/README.txt) for end-user details.

## How it works

1. On startup the app attaches to the Windows network stack through
   [WinDivert](https://reqrypt.org/windivert.html), a user-mode
   capture / re-inject API backed by a signed WFP helper driver. The
   filter `outbound and ip` covers every outbound IP packet (TCP, UDP,
   ICMP, IPv4, IPv6).
2. A dedicated worker thread calls `WinDivertRecvEx` / `WinDivertSendEx`
   in batches (up to `WINDIVERT_BATCH_MAX` packets per syscall).
3. Per batch, one atomic read of a single `volatile LONG` picks the
   path:
   - **CONNECTED** → forward the batch untouched (zero added latency).
   - **REFRESHING** → hold the batch so the connection can settle.
4. The toggle (big button, or Left Alt tap) flips that flag with a
   single `InterlockedExchange`. The very next outbound packet picks up
   the new state — microseconds later.

There are no route changes, no firewall-rule edits, no adapter
disable/enable. It's a live gate in front of the outbound packet queue.

```
 user app / OS
      │  outbound packet
      ▼
┌──────────────┐    WinDivertRecvEx (batched)     ┌────────────────────────┐
│ Windows TCP/ │ ───────────────────────────────▶ │ ethernetswitch         │
│ IP stack     │                                  │  atomic flag check     │
│ (via WFP /   │ ◀───────────── WinDivertSendEx ─ │  ├─ CONNECTED → forward│
│  WinDivert)  │   (only while CONNECTED)         │  └─ REFRESHING → hold  │
└──────────────┘                                  └────────────────────────┘
```

### Why a low-level keyboard hook

`RegisterHotKey()` won't bind a bare modifier, so we use a
`WH_KEYBOARD_LL` low-level keyboard hook on a dedicated thread. The hook
fires on the very first keydown edge of `VK_LMENU` (Left Alt),
suppresses the OS-level auto-repeat stream that would otherwise re-fire
the toggle, and swallows both the keydown and the keyup so the Alt key
doesn't leak through and flash Windows' menu bar. The actual toggle is a
single `InterlockedExchange` on the shared flag — the divert worker
reads it on its next batch (it's blocked in `WinDivertRecvEx` waiting
for the next outbound packet, which is exactly when the flag matters),
so the effect is applied to the first packet after the keydown edge.

## Building from source

### Prerequisites

- **Windows 10 or 11** (x86 or x64)
- Either **Visual Studio 2019+** with the C++ build tools, **or**
  a **MinGW-w64** toolchain (MSYS2's `mingw-w64-x86_64-toolchain`, or
  the `mingw-w64` package on Linux for cross-compiling)
- **PowerShell** (built in on Windows) — used once to fetch the
  WinDivert SDK

### 1. Fetch the WinDivert SDK

WinDivert ships with a signed kernel helper driver; we use the official
pre-built release rather than rebuilding it ourselves:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\fetch_windivert.ps1
```

This downloads `WinDivert-2.2.2-A.zip` from the upstream GitHub release
and lays it out under `third_party\windivert\`.

### 2a. Build with MSVC

From an **x64 Native Tools Command Prompt for VS**:

```cmd
build.bat           :: 64-bit
build.bat x86       :: 32-bit
```

### 2b. Build with MinGW-w64

```sh
make                # 64-bit
make ARCH=x86       # 32-bit
```

Either path produces `build\ethernetswitch.exe` alongside the matching
`WinDivert.dll` and `WinDivert64.sys` / `WinDivert32.sys`. These three
files must stay in the same directory.

## Running

Double-click `ethernetswitch.exe`. Windows will prompt for Administrator
— the embedded manifest requests elevation because the WinDivert helper
driver can't load without it.

| Action                                  | Effect                               |
| --------------------------------------- | ------------------------------------ |
| Click the big button                    | Toggle refresh / resume              |
| Press **Left Alt** (from anywhere)      | Toggle refresh / resume              |
| Close the window                        | Restore normal forwarding, unload    |

The status label shows whether the connection is **ACTIVE** (green) or
**REFRESHING** (amber), plus a running count of packets forwarded and
buffered.

## Notes

- **Outbound only**, by design. Inbound traffic is never touched.
- Windows considers localhost (loopback) traffic to be "outbound" at
  the WinDivert layer, so it gets paused too while refreshing. If you
  want to leave loopback traffic flowing, change the filter in
  `src/main.c` from `"outbound and ip"` to
  `"outbound and ip and not loopback"` and rebuild.
- While refreshing, existing connections see a pause (they don't get
  RST'd). Most applications are tolerant of this for short periods.
- Closing the app (or killing the process) restores normal networking
  immediately: WinDivert tears the handle down and the kernel stack
  goes back to sending packets itself. There is no cleanup step to
  forget.

## Repo layout

```
src/
  main.c                  core program (WinDivert worker + Win32 GUI + LL kbd hook)
  ethernetswitch.rc       resource file (manifest + version info)
  ethernetswitch.manifest UAC manifest (requires Administrator)
scripts/
  fetch_windivert.ps1     downloads and unpacks the signed WinDivert SDK
release/
  x64/, x86/              pre-built ready-to-run binaries
  run.bat                 double-clickable arch-picker
  README.txt              end-user docs
build.bat                 MSVC build
Makefile                  MinGW-w64 build
```

## License

The code in this repository is released under the MIT license.
WinDivert itself is licensed under LGPLv3 — see
[the WinDivert license](https://github.com/basil00/WinDivert/blob/master/LICENSE).
