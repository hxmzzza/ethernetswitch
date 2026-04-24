ethernetswitch - ready-to-run build
===================================

This folder contains pre-built, ready-to-run binaries. Just start the program.

How to run
----------

On 64-bit Windows (almost everything today):
  1. Open the  x64\  folder.
  2. Double-click  ethernetswitch.exe
  3. Click "Yes" on the UAC prompt (WinDivert's kernel driver needs Admin).

On 32-bit Windows:
  1. Open the  x86\  folder.
  2. Double-click  ethernetswitch.exe

Not sure which? Right-click "This PC" -> Properties -> System type.
If it says "64-bit operating system", use  x64\ .

Using the switch
----------------

  - Click the big button -------> toggle outbound traffic on/off
  - Press SPACE (window focused)-> toggle outbound traffic on/off
  - Press F9 from ANY app ------> toggle outbound traffic on/off (global hotkey)
  - Close the window -----------> network returns to normal immediately

When "OFFLINE", every outbound packet (TCP, UDP, ICMP, IPv4, IPv6, loopback)
is dropped by the kernel before it reaches the wire. Existing connections
will stall and time out; new ones can't even send a SYN.

When "ONLINE", packets pass through untouched with effectively zero added
latency -- the same approach clumsy uses.

What's in each folder
---------------------

  ethernetswitch.exe   the app itself
  WinDivert.dll        user-mode library (must sit next to the .exe)
  WinDivert64.sys      signed kernel driver  (x64 build)
  WinDivert32.sys      signed kernel driver  (x86 build)

The WinDivert files are the official signed release from
https://github.com/basil00/WinDivert (LGPL-3.0). They must stay next to
ethernetswitch.exe -- do not separate them.

Uninstall
---------

Delete the folder. If you want to also remove the kernel driver service
that WinDivert silently installs on first use, open an Administrator
command prompt and run:

    sc stop WinDivert
    sc delete WinDivert

or reboot -- the driver unloads automatically when no process is using it.
