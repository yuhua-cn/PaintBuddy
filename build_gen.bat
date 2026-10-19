@echo off
REM ============================================================
REM  Build + run the synthetic data generator (console exe).
REM  Produces realistic painting-session JSONL into an ISOLATED dir
REM  (build\sampledata), never into the user's real data dir.
REM  ASCII-only; avoid parenthesized blocks.
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
set "OBJDIR=%OUT%\gen_obj"
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OBJDIR%" mkdir "%OBJDIR%"

set "CXXFLAGS=/nologo /std:c++17 /EHsc /W3 /O2 /MT /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /Isrc /Fo%OBJDIR%\ /c"
set "UNITS=tests\gen_sample.cpp src\Config.cpp src\Store.cpp src\Json.cpp"

set FAILED=0
echo ==== COMPILING GENERATOR ====
for %%f in (%UNITS%) do (
    echo   %%f
    cl %CXXFLAGS% "%%f"
    if errorlevel 1 set FAILED=1
)
if "%FAILED%"=="1" goto :compile_failed

echo.
echo ==== LINKING GENERATOR ====
set OBJS=
for %%f in ("%OBJDIR%\*.obj") do set "OBJS=!OBJS! %%f"
link /nologo /SUBSYSTEM:CONSOLE /INCREMENTAL:NO /OUT:%OUT%\gen_sample.exe !OBJS! user32.lib advapi32.lib shell32.lib
if errorlevel 1 goto :link_failed

echo.
echo ==== RUNNING GENERATOR ====
"%OUT%\gen_sample.exe"
set RC=%ERRORLEVEL%
endlocal
exit /b %RC%

:compile_failed
echo [FAILED] generator compile errors.
endlocal
exit /b 1
:link_failed
echo [FAILED] generator link errors.
endlocal
exit /b 1
