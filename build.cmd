@echo off
rem SPDX-License-Identifier: MIT
rem
rem gudprobe.exe -- needs the Windows SDK and nothing else. Run from a
rem "x64 Native Tools Command Prompt".
rem
rem The driver is not built here. It needs the WDK, and the two version-specific
rem lines in src\driver\GudDisplay.inf (UmdfExtensions, UmdfLibraryVersion) must
rem match the WDK installed -- copy both from the IndirectDisplay sample that
rem ships with it, which is guaranteed to agree with its own headers.

setlocal
if not exist build mkdir build

cl /nologo /W4 /O2 /MT /I include ^
   src\probe\gudprobe.c ^
   src\probe\winusb_transport.c ^
   src\common\gud_host.c ^
   src\common\modeline.c ^
   src\common\convert.c ^
   src\common\lz4enc.c ^
   /Fo:build\ /Fe:build\gudprobe.exe ^
   winusb.lib setupapi.lib

if errorlevel 1 exit /b 1
echo.
echo built build\gudprobe.exe
