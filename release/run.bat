@echo off
REM Double-click me to launch ethernetswitch with the right architecture.
REM (Picks x64 on 64-bit Windows, x86 otherwise.)

setlocal
set HERE=%~dp0

if defined PROCESSOR_ARCHITEW6432 goto :x64
if /I "%PROCESSOR_ARCHITECTURE%"=="AMD64" goto :x64
if /I "%PROCESSOR_ARCHITECTURE%"=="ARM64" goto :x64
goto :x86

:x64
start "" "%HERE%x64\ethernetswitch.exe"
exit /b 0

:x86
start "" "%HERE%x86\ethernetswitch.exe"
exit /b 0
