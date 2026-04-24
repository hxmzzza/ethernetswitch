# ethernetswitch

A tiny Windows tool in the spirit of [clumsy](https://jagt.github.io/clumsy/)
that does one thing, instantly:

> **Toggle every outbound packet leaving this machine on or off.**

Click the big button (or press **F9** from anywhere, or **Space** when the
window is focused) and all outbound IP traffic — TCP, UDP, ICMP, IPv4,
IPv6, loopback — is dropped at the kernel layer. Click again and traffic
resumes. The switch is per-packet: the very next outbound packet after a
toggle observes the new state, so it is as instant as clumsy's own
filtering.

## How it works (and why it's instant)

Just like clumsy, ethernetswitch is built on
[WinDivert](https://reqrypt.org/windivert.html), a user-mode
capture/modify/re-inject API backed by a signed WFP kernel driver.

1. On startup we open a single divert handle with the filter `outbound and ip`
   at `WINDIVERT_LAYER_NETWORK`. From that point on, every outbound packet
   is pulled out of the Windows network stack and queued for us.
2. A dedicated worker thread calls `WinDivertRecvEx` in batches (up to
   `WINDIVERT_BATCH_MAX` packets per syscall). Per batch it reads one
   atomic flag:
   - **PASS** → the whole batch is re-injected with `WinDivertSendEx`
     unchanged. Zero modification, zero added latency.
   - **BLOCK** → nothing happens. Because the packets were already
     diverted out of the stack, "do nothing" means "the packets never
     leave this host."
3. Toggling flips that atomic flag with a single `InterlockedExchange`.
   Any outbound packet already in flight in the kernel queue is the only
   thing that could possibly slip past; on modern Windows that's at most
   one batch, typically microseconds.

No route changes, no firewall rules, no `netsh interface set interface`
up/down bounce, no adapter disable. Just a live packet gate.

```
 user app / OS
      │  outbound packet
      ▼
┌──────────────┐    WinDivertRecvEx (batched)     ┌────────────────────┐
│ Windows TCP/ │ ───────────────────────────────▶ │ ethernetswitch     │
│ IP stack     │                                  │  atomic flag check │
│ (via WFP /   │ ◀───────────── WinDivertSendEx ─ │  ├─ PASS → reinject│
│  WinDivert)  │   (only when flag == PASS)       │  └─ BLOCK → drop   │
└──────────────┘                                  └────────────────────┘
```

## Running the pre-built binary (no build required)

A ready-to-run build is checked in under [`release/`](./release/). On Windows:

1. Download the repo (or just the `release/` folder).
2. Double-click `release/run.bat` — it picks the right architecture
   automatically — or drill in to `release/x64/ethernetswitch.exe`
   (use `x86` on 32-bit Windows).
3. Accept the UAC prompt. That's it.

See [`release/README.txt`](./release/README.txt) for details.

## Building from source

### Prerequisites

- **Windows 10 or 11** (x86 or x64)
- Either **Visual Studio 2019+** with the C++ build tools, **or**
  a **MinGW-w64** toolchain (MSYS2's `mingw-w64-x86_64-toolchain`)
- **PowerShell** (built in) — used once to fetch the WinDivert SDK

### 1. Fetch the WinDivert SDK

The WinDivert driver must be code-signed to load on stock Windows. We use
the official pre-built (and signed) release instead of building the
driver ourselves:

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
(the embedded manifest requests elevation; WinDivert's driver cannot be
loaded without it).

| Action                                 | Effect                             |
| -------------------------------------- | ---------------------------------- |
| Click the big button                   | Toggle outbound traffic            |
| **Space** (window focused)             | Toggle outbound traffic            |
| **F9** (from anywhere, global hotkey)  | Toggle outbound traffic            |
| Close the window                       | Restore normal networking, unload  |

The status label shows whether packets are currently **PASSING** (green)
or **BLOCKED** (red), plus a running count of packets passed / dropped.

## Notes and caveats

- **Outbound only**, by design. clumsy lets you scope inbound vs outbound
  via filters; this tool is deliberately the big red button for
  *outbound* traffic. Inbound packets are never touched.
- **Loopback (127.0.0.1) counts as outbound** on Windows per
  WinDivert's own documentation, so it is blocked too. If you want to
  keep localhost alive while killing external traffic, change the filter
  in `src/main.c` from `"outbound and ip"` to `"outbound and ip and not loopback"`.
- Because we drop packets *before* they reach the network, the blocking
  side looks like a cable unplug to the apps on your box — connections
  stall and eventually time out; they don't get RST'd. This matches what
  clumsy's drop function does.
- Closing the app (or killing the process) stops diverting immediately:
  WinDivert tears the handle down and the kernel stack goes back to
  sending packets normally. There is nothing to "undo."

## Repo layout

```
src/
  main.c                  core program (WinDivert worker + Win32 GUI)
  ethernetswitch.rc       resource file (manifest + version info)
  ethernetswitch.manifest UAC manifest (requires Administrator)
scripts/
  fetch_windivert.ps1     downloads and unpacks the signed WinDivert SDK
build.bat                 MSVC build
Makefile                  MinGW-w64 build
```

## License

The code in this repository is released under the MIT license.
WinDivert itself is licensed under LGPLv3 — see
[the WinDivert license](https://github.com/basil00/WinDivert/blob/master/LICENSE).
