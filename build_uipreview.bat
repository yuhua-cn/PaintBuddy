@echo off
REM ============================================================
REM  Build + run the headless UI preview (console exe).
REM  Links ALL units except main.cpp (no wWinMain) so App can be
REM  driven offscreen to render each page to PNG.
REM  Needs /bigobj + cppwinrt headers (Recorder uses C++/WinRT).
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

set "CPPWINRT_INC=%WindowsSdkDir%Include\%WindowsSdkVer%cppwinrt"
if exist "%CPPWINRT_INC%\winrt\base.h" goto :have_cppwinrt
set "CPPWINRT_INC=%WindowsSdkDir%Include\10.0.26100.0\cppwinrt"
if exist "%CPPWINRT_INC%\winrt\base.h" goto :have_cppwinrt
echo [ERROR] cppwinrt headers not found.
exit /b 1
:have_cppwinrt
set "INCLUDE=%CPPWINRT_INC%;%INCLUDE%"
goto :build

:no_vswhere
echo [ERROR] vswhere.exe not found.
exit /b 1
:no_vcvars
echo [ERROR] vcvars64 failed.
exit /b 1

:build
set "OUT=build"
set "OBJDIR=%OUT%\uiprev_obj"
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OBJDIR%" mkdir "%OBJDIR%"

set "CXXFLAGS=/nologo /std:c++17 /EHsc /W3 /O2 /MT /utf-8 /bigobj /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /Isrc /Fo%OBJDIR%\ /c"
set "LIBS=user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib advapi32.lib d2d1.lib dwrite.lib d3d11.lib dxgi.lib dwmapi.lib shlwapi.lib windowscodecs.lib mf.lib mfplat.lib mfreadwrite.lib mfuuid.lib runtimeobject.lib windowsapp.lib hid.lib winmm.lib comctl32.lib uxtheme.lib propsys.lib"

set "UNITS=tests\ui_preview.cpp src\MainWindow.cpp src\Capture.cpp src\Recorder.cpp src\PenInput.cpp src\PenOverrides.cpp src\PenInject.cpp src\Charts.cpp src\Ui.cpp src\Analyzer.cpp src\Config.cpp src\Store.cpp src\Json.cpp src\Video.cpp"

set FAILED=0
echo ==== COMPILING UI PREVIEW ====
for %%f in (%UNITS%) do (
    echo   %%f
    cl %CXXFLAGS% "%%f"
    if errorlevel 1 set FAILED=1
)
if "%FAILED%"=="1" goto :compile_failed

echo.
echo ==== LINKING UI PREVIEW ====
set OBJS=
for %%f in ("%OBJDIR%\*.obj") do set "OBJS=!OBJS! %%f"
link /nologo /SUBSYSTEM:CONSOLE /INCREMENTAL:NO /OUT:%OUT%\ui_preview.exe !OBJS! %LIBS%
if errorlevel 1 goto :link_failed

echo.
echo ==== RUNNING UI PREVIEW ====
"%OUT%\ui_preview.exe"
set RC=%ERRORLEVEL%
endlocal
exit /b %RC%

:compile_failed
echo [FAILED] ui preview compile errors.
endlocal
exit /b 1
:link_failed
echo [FAILED] ui preview link errors.
endlocal
exit /b 1
