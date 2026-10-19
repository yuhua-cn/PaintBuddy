@echo off
REM ============================================================
REM  Build the pen diagnostics helper (console exe).
REM  Enumerates digitizer devices, registers Raw Input, and counts
REM  reports across three channels: Raw Input HID, WM_POINTER, injected mouse.
REM  ASCII-only on purpose: cmd.exe parses .bat in the system ANSI codepage,
REM  so any non-ASCII byte here corrupts the parser.
REM ============================================================
setlocal EnableDelayedExpansion
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :no_vswhere
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH goto :no_vswhere
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 goto :no_vcvars
goto :build

:no_vswhere
echo [ERROR] vswhere.exe not found.
exit /b 1
:no_vcvars
echo [ERROR] vcvars64 failed.
exit /b 1

:build
set "OUT=build"
if not exist "%OUT%" mkdir "%OUT%"
echo ==== COMPILING PEN DIAG ====
cl /nologo /std:c++17 /EHsc /W3 /O2 /MT /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /Isrc tests\pen_diag.cpp /Fe:%OUT%\pen_diag.exe /Fo:%OUT%\ user32.lib gdi32.lib hid.lib
if errorlevel 1 goto :compile_failed
echo.
echo [OK] %OUT%\pen_diag.exe
echo Usage: build\pen_diag.exe [seconds]   then draw with the pen during that time
endlocal
exit /b 0

:compile_failed
echo [FAILED] pen_diag compile errors.
endlocal
exit /b 1
