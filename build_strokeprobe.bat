@echo off
REM ============================================================
REM  Build the stroke-anatomy probe (console exe).
REM  Read-only diagnostic: loads real logs, prints stroke stats.
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
set "OBJDIR=%OUT%\strokeprobe_obj"
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OBJDIR%" mkdir "%OBJDIR%"

set "CXXFLAGS=/nologo /std:c++17 /EHsc /W3 /O2 /MT /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /Isrc /Fo%OBJDIR%\ /c"

set "UNITS=tests\stroke_probe.cpp src\Analyzer.cpp src\Config.cpp src\Store.cpp src\Json.cpp"

set FAILED=0
echo.
echo ==== COMPILING PROBE ====
for %%f in (%UNITS%) do (
    echo   %%f
    cl %CXXFLAGS% "%%f"
    if errorlevel 1 set FAILED=1
)
if "%FAILED%"=="1" goto :compile_failed

echo.
echo ==== LINKING PROBE ====
set OBJS=
for %%f in ("%OBJDIR%\*.obj") do set "OBJS=!OBJS! %%f"
link /nologo /SUBSYSTEM:CONSOLE /INCREMENTAL:NO /OUT:%OUT%\stroke_probe.exe !OBJS! user32.lib advapi32.lib shell32.lib
if errorlevel 1 goto :link_failed

echo.
echo ==== RUNNING PROBE ====
"%OUT%\stroke_probe.exe" %*
set RC=%ERRORLEVEL%
endlocal
exit /b %RC%

:compile_failed
echo.
echo [FAILED] probe compile errors.
endlocal
exit /b 1
:link_failed
echo.
echo [FAILED] probe link errors.
endlocal
exit /b 1
