@echo off
REM Build + run the Video-module isolation probe (console exe).
REM Links Video.cpp (needs cppwinrt headers) + Store/Config/Json.
setlocal EnableDelayedExpansion
cd /d "%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
set "CPPWINRT_INC=%WindowsSdkDir%Include\%WindowsSdkVer%cppwinrt"
if not exist "%CPPWINRT_INC%\winrt\base.h" set "CPPWINRT_INC=%WindowsSdkDir%Include\10.0.26100.0\cppwinrt"
set "INCLUDE=%CPPWINRT_INC%;%INCLUDE%"
set "OBJDIR=build\vprobe_obj"
if not exist build mkdir build
if not exist "%OBJDIR%" mkdir "%OBJDIR%"
set "CXXFLAGS=/nologo /std:c++17 /EHsc /W3 /O2 /MT /utf-8 /bigobj /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /Isrc /Fo%OBJDIR%\ /c"
set "UNITS=tests\video_probe.cpp src\Video.cpp src\Store.cpp src\Config.cpp src\Json.cpp"
set FAILED=0
for %%f in (%UNITS%) do (
    cl %CXXFLAGS% "%%f"
    if errorlevel 1 set FAILED=1
)
if "%FAILED%"=="1" (echo [COMPILE FAILED] & exit /b 1)
set OBJS=
for %%f in ("%OBJDIR%\*.obj") do set "OBJS=!OBJS! %%f"
link /nologo /SUBSYSTEM:CONSOLE /INCREMENTAL:NO /OUT:build\video_probe.exe !OBJS! user32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib windowscodecs.lib mf.lib mfplat.lib mfreadwrite.lib mfuuid.lib shlwapi.lib propsys.lib
if errorlevel 1 (echo [LINK FAILED] & exit /b 1)
echo ==== RUN video_probe ====
build\video_probe.exe
echo EXIT=%ERRORLEVEL%
endlocal
exit /b 0
