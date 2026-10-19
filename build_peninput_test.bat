@echo off
REM ============================================================
REM  Build the headless pen-input parser test (console exe).
REM  Links only PenInput + Store + Json; no GUI / capture / record.
REM  No real digitizer hardware needed: HID value fetching is
REM  replaced by an injected mock (see tests\test_peninput.cpp).
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
set "OBJDIR=%OUT%\peninput_test_obj"
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OBJDIR%" mkdir "%OBJDIR%"

set "CXXFLAGS=/nologo /std:c++17 /EHsc /W3 /O2 /MT /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /Isrc /Fo%OBJDIR%\ /c"

set "UNITS=tests\test_peninput.cpp src\PenInput.cpp src\PenOverrides.cpp src\PenInject.cpp src\Store.cpp src\Json.cpp"

set FAILED=0
echo.
echo ==== COMPILING PEN INPUT TEST ====
for %%f in (%UNITS%) do (
    echo   %%f
    cl %CXXFLAGS% "%%f"
    if errorlevel 1 set FAILED=1
)
if "%FAILED%"=="1" goto :compile_failed

echo.
echo ==== LINKING PEN INPUT TEST ====
set OBJS=
for %%f in ("%OBJDIR%\*.obj") do set "OBJS=!OBJS! %%f"
link /nologo /SUBSYSTEM:CONSOLE /INCREMENTAL:NO /OUT:%OUT%\test_peninput.exe !OBJS! hid.lib user32.lib advapi32.lib shell32.lib
if errorlevel 1 goto :link_failed

echo.
echo ==== RUNNING PEN INPUT TEST ====
"%OUT%\test_peninput.exe"
set RC=%ERRORLEVEL%
echo.
if "%RC%"=="0" (echo [TEST OK]) else (echo [TEST FAILED] exit=%RC%)
endlocal
exit /b %RC%

:compile_failed
echo.
echo [FAILED] pen input test compile errors.
endlocal
exit /b 1
:link_failed
echo.
echo [FAILED] pen input test link errors.
endlocal
exit /b 1
