@echo off
setlocal EnableDelayedExpansion
REM ---------------------------------------------------------------------------
REM Builds ethernetswitch.exe with MSVC (cl.exe). Run from an
REM "x64 Native Tools Command Prompt for VS" shell so that cl, rc, and link
REM are on PATH.
REM
REM Prereq: run `powershell -ExecutionPolicy Bypass -File scripts\fetch_windivert.ps1`
REM first, which populates third_party\windivert\.
REM ---------------------------------------------------------------------------

set ROOT=%~dp0
set OUT=%ROOT%build
set WD=%ROOT%third_party\windivert

if not exist "%WD%\include\windivert.h" (
    echo [!] WinDivert not found. Run:
    echo     powershell -ExecutionPolicy Bypass -File scripts\fetch_windivert.ps1
    exit /b 1
)

if not exist "%OUT%" mkdir "%OUT%"

set ARCH=x64
if /I "%1"=="x86" set ARCH=x86

echo [*] Building for %ARCH% ...

rc /nologo /i "%ROOT%src" /fo "%OUT%\ethernetswitch.res" "%ROOT%src\ethernetswitch.rc"
if errorlevel 1 exit /b 1

cl /nologo /W3 /O2 /MT ^
   /I "%WD%\include" ^
   "%ROOT%src\main.c" ^
   "%OUT%\ethernetswitch.res" ^
   /link ^
   /SUBSYSTEM:WINDOWS ^
   /LIBPATH:"%WD%\%ARCH%" ^
   WinDivert.lib user32.lib gdi32.lib comctl32.lib advapi32.lib ^
   /OUT:"%OUT%\ethernetswitch.exe"
if errorlevel 1 exit /b 1

copy /Y "%WD%\%ARCH%\WinDivert.dll"   "%OUT%\" >nul
if /I "%ARCH%"=="x64" (
    copy /Y "%WD%\x64\WinDivert64.sys" "%OUT%\" >nul
) else (
    copy /Y "%WD%\x86\WinDivert32.sys" "%OUT%\" >nul
)

echo.
echo [+] Built: %OUT%\ethernetswitch.exe
echo     (WinDivert.dll and WinDivert*.sys copied next to it)
endlocal
