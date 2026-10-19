@echo off
REM ============================================================
REM  Build + run the WGC frame-pool scaling probe (console exe).
REM  Standalone: does NOT compile any src\*.cpp product unit, so
REM  it cannot affect the shipping recorder.
REM  ASCII-only; avoid parenthesized blocks (SDK path has "(x86)").
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
set "OBJDIR=%OUT%\poolprobe_obj"
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OBJDIR%" mkdir "%OBJDIR%"

set "CXXFLAGS=/nologo /std:c++17 /EHsc /W3 /O2 /MT /utf-8 /bigobj /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /Isrc /Fo%OBJDIR%\ /c"
set "LIBS=user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib advapi32.lib d3d11.lib dxgi.lib runtimeobject.lib windowsapp.lib"

echo ==== COMPILING POOL PROBE ====
cl %CXXFLAGS% "tests\probe_pool.cpp"
if errorlevel 1 goto :compile_failed

echo.
echo ==== LINKING POOL PROBE ====
set OBJS=
for %%f in ("%OBJDIR%\*.obj") do set "OBJS=!OBJS! %%f"
link /nologo /SUBSYSTEM:CONSOLE /INCREMENTAL:NO /OUT:%OUT%\probe_pool.exe !OBJS! %LIBS%
if errorlevel 1 goto :link_failed

echo.
echo ==== RUNNING POOL PROBE ====
"%OUT%\probe_pool.exe"
set RC=%ERRORLEVEL%
endlocal
exit /b %RC%

:compile_failed
echo [FAILED] pool probe compile errors.
endlocal
exit /b 1
:link_failed
echo [FAILED] pool probe link errors.
endlocal
exit /b 1
