ethernetswitch - ready-to-run build
===================================

Outbound connection refresher for Windows. Pauses and releases outbound
ethernet traffic on demand so stale TCP state can drain and the
connection comes back cleaner.

How to run
----------

On 64-bit Windows (almost everything today):
  1. Open the  x64\  folder.
  2. Double-click  ethernetswitch.exe
  3. Click "Yes" on the UAC prompt (the WinDivert helper driver that
     ethernetswitch uses to attach to the network stack requires
     Administrator access).

On 32-bit Windows:
  1. Open the  x86\  folder.
  2. Double-click  ethernetswitch.exe

Not sure which? Right-click "This PC" -> Properties -> System type.
If it says "64-bit operating system", use  x64\ .

Using the refresher
-------------------

  - Click the big button ---------------> start / stop a refresh
  - Press LEFT ALT from any app --------> start / stop a refresh (global)

The global shortcut fires the instant Left Alt goes down, from any
foreground window. While "REFRESHING", outbound traffic is paused so
the network stack can settle. Press Left Alt (or click the button)
again to resume -- traffic is forwarded again immediately. Closing the
window always returns the connection to normal.

What's in each folder
---------------------

  ethernetswitch.exe   the app itself
  WinDivert.dll        user-mode library (must sit next to the .exe)
  WinDivert64.sys      signed helper driver  (x64 build)
  WinDivert32.sys      signed helper driver  (x86 build)

The WinDivert files are the official signed release from
https://github.com/basil00/WinDivert (LGPL-3.0). They must stay next to
ethernetswitch.exe -- do not separate them.

Uninstall
---------

Delete the folder. If you want to also remove the helper driver service
that WinDivert silently installs on first use, open an Administrator
command prompt and run:

    sc stop WinDivert
    sc delete WinDivert

or reboot -- the driver unloads automatically when no process is using it.
