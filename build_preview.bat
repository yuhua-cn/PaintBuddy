@echo off
REM ============================================================
REM  Build + run the chart preview renderer (console exe).
REM  Loads real data -> analyzes -> renders iOS-style charts to PNG.
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
set "OBJDIR=%OUT%\prev_obj"
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OBJDIR%" mkdir "%OBJDIR%"

set "CXXFLAGS=/nologo /std:c++17 /EHsc /W3 /O2 /MT /utf-8 /bigobj /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /Isrc /Fo%OBJDIR%\ /c"
set "UNITS=tests\render_preview.cpp src\Analyzer.cpp src\Charts.cpp src\Ui.cpp src\Config.cpp src\Store.cpp src\Json.cpp"
set "LIBS=user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib advapi32.lib d2d1.lib dwrite.lib windowscodecs.lib shlwapi.lib"

set FAILED=0
echo ==== COMPILING PREVIEW ====
for %%f in (%UNITS%) do (
    echo   %%f
    cl %CXXFLAGS% "%%f"
    if errorlevel 1 set FAILED=1
)
if "%FAILED%"=="1" goto :compile_failed

echo.
echo ==== LINKING PREVIEW ====
set OBJS=
for %%f in ("%OBJDIR%\*.obj") do set "OBJS=!OBJS! %%f"
link /nologo /SUBSYSTEM:CONSOLE /INCREMENTAL:NO /OUT:%OUT%\render_preview.exe !OBJS! %LIBS%
if errorlevel 1 goto :link_failed

echo.
echo ==== RUNNING PREVIEW ====
"%OUT%\render_preview.exe"
set RC=%ERRORLEVEL%
endlocal
exit /b %RC%

:compile_failed
echo [FAILED] preview compile errors.
endlocal
exit /b 1
:link_failed
echo [FAILED] preview link errors.
endlocal
exit /b 1
