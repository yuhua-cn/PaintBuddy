@echo off
REM ============================================================
REM  Build the pointer-device property probe (console exe).
REM  Single-file tool; no project sources linked.
REM  Keep ASCII-only; avoid parenthesized blocks (SDK path has "(x86)").
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
set "OBJDIR=%OUT%\prop_obj"
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OBJDIR%" mkdir "%OBJDIR%"

set "CXXFLAGS=/nologo /std:c++17 /EHsc /W3 /O2 /MT /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /Fo%OBJDIR%\ /c"

echo.
echo ==== COMPILING PROP PROBE ====
cl %CXXFLAGS% tests\prop_probe.cpp
if errorlevel 1 goto :compile_failed

echo.
echo ==== LINKING PROP PROBE ====
link /nologo /SUBSYSTEM:CONSOLE /INCREMENTAL:NO /OUT:%OUT%\prop_probe.exe %OBJDIR%\prop_probe.obj hid.lib user32.lib
if errorlevel 1 goto :link_failed

echo.
echo [OK] %OUT%\prop_probe.exe
endlocal
exit /b 0

:compile_failed
echo.
echo [FAILED] prop probe compile errors.
endlocal
exit /b 1
:link_failed
echo.
echo [FAILED] prop probe link errors.
endlocal
exit /b 1
