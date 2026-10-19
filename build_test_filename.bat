@echo off
REM ============================================================
REM  Build + run the filename-extraction test (console exe).
REM  Recorder.cpp pulls in C++/WinRT + Media Foundation, so this
REM  needs /bigobj, the cppwinrt include dir, and the media libs.
REM  ASCII-only; avoid parenthesized blocks (SDK path has "(x86)").
REM
REM  NOTE: Recorder.cpp references Capture (Capture::I / Snapshot /
REM  FgSnapshot) and DocFolderPath (Video.cpp), which in turn pull in
REM  PenInput / PenInject / PenOverrides. So we link every src unit
REM  except main.cpp (its entry point would clash with the test main).
REM  LIBS mirrors build.bat.
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
set "OBJDIR=%OUT%\fname_obj"
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OBJDIR%" mkdir "%OBJDIR%"

set "CXXFLAGS=/nologo /std:c++17 /EHsc /W3 /O2 /MT /utf-8 /bigobj /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /Isrc /Fo%OBJDIR%\ /c"
set "LIBS=user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib advapi32.lib d2d1.lib dwrite.lib d3d11.lib dxgi.lib dwmapi.lib shlwapi.lib windowscodecs.lib mf.lib mfplat.lib mfreadwrite.lib mfuuid.lib runtimeobject.lib windowsapp.lib hid.lib winmm.lib comctl32.lib uxtheme.lib propsys.lib"
set "UNITS=tests\test_filename.cpp src\Analyzer.cpp src\Capture.cpp src\Charts.cpp src\Config.cpp src\Json.cpp src\MainWindow.cpp src\PenInject.cpp src\PenInput.cpp src\PenOverrides.cpp src\Recorder.cpp src\Store.cpp src\Ui.cpp src\Video.cpp"

set FAILED=0
echo ==== COMPILING FILENAME TEST ====
for %%f in (%UNITS%) do (
    echo   %%f
    cl %CXXFLAGS% "%%f"
    if errorlevel 1 set FAILED=1
)
if "%FAILED%"=="1" goto :compile_failed

echo.
echo ==== LINKING FILENAME TEST ====
set OBJS=
for %%f in ("%OBJDIR%\*.obj") do set "OBJS=!OBJS! %%f"
link /nologo /SUBSYSTEM:CONSOLE /INCREMENTAL:NO /OUT:%OUT%\test_filename.exe !OBJS! %LIBS%
if errorlevel 1 goto :link_failed

echo.
echo ==== RUNNING FILENAME TEST ====
"%OUT%\test_filename.exe"
set RC=%ERRORLEVEL%
endlocal
exit /b %RC%

:compile_failed
echo [FAILED] filename test compile errors.
endlocal
exit /b 1
:link_failed
echo [FAILED] filename test link errors.
endlocal
exit /b 1
