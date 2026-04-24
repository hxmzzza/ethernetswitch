<#
  Downloads the official pre-built WinDivert 2.2 SDK (driver + DLL + headers).
  The driver binaries inside this release are digitally signed by the
  WinDivert project, which is what allows the kernel driver to load on
  Windows 10/11 without developer mode.

  Output layout:
    third_party/windivert/
      include/windivert.h
      x64/WinDivert.dll
      x64/WinDivert.lib
      x64/WinDivert64.sys
      x86/WinDivert.dll
      x86/WinDivert.lib
      x86/WinDivert32.sys
#>

param(
    [string]$Version = "2.2.2-A",
    [string]$OutDir  = (Join-Path $PSScriptRoot "..\third_party\windivert")
)

$ErrorActionPreference = "Stop"

$zipName = "WinDivert-$Version.zip"
$url     = "https://github.com/basil00/WinDivert/releases/download/v$Version/$zipName"

$tmp    = Join-Path $env:TEMP "ethernetswitch_$Version"
$zipPath = Join-Path $tmp $zipName
New-Item -ItemType Directory -Force -Path $tmp    | Out-Null
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

Write-Host "Downloading $url ..."
Invoke-WebRequest -Uri $url -OutFile $zipPath -UseBasicParsing

Write-Host "Extracting ..."
$extractDir = Join-Path $tmp "extract"
if (Test-Path $extractDir) { Remove-Item -Recurse -Force $extractDir }
Expand-Archive -Path $zipPath -DestinationPath $extractDir -Force

$root = Get-ChildItem -Path $extractDir -Directory | Select-Object -First 1
if (-not $root) { throw "WinDivert archive layout unexpected" }

New-Item -ItemType Directory -Force -Path (Join-Path $OutDir "include") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $OutDir "x64")     | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $OutDir "x86")     | Out-Null

Copy-Item (Join-Path $root.FullName "include\windivert.h")   (Join-Path $OutDir "include\windivert.h") -Force

Copy-Item (Join-Path $root.FullName "x64\WinDivert.dll")     (Join-Path $OutDir "x64\WinDivert.dll")   -Force
Copy-Item (Join-Path $root.FullName "x64\WinDivert.lib")     (Join-Path $OutDir "x64\WinDivert.lib")   -Force
Copy-Item (Join-Path $root.FullName "x64\WinDivert64.sys")   (Join-Path $OutDir "x64\WinDivert64.sys") -Force

Copy-Item (Join-Path $root.FullName "x86\WinDivert.dll")     (Join-Path $OutDir "x86\WinDivert.dll")   -Force
Copy-Item (Join-Path $root.FullName "x86\WinDivert.lib")     (Join-Path $OutDir "x86\WinDivert.lib")   -Force
Copy-Item (Join-Path $root.FullName "x86\WinDivert32.sys")   (Join-Path $OutDir "x86\WinDivert32.sys") -Force

Write-Host "WinDivert $Version installed to $OutDir"
