@echo off
REM ============================================================
REM  PaintingBuddy build script (MSVC x64)
REM  Usage:  build.bat          incremental
REM          build.bat rebuild  full rebuild
REM
REM  NOTE: keep this file ASCII-only, and avoid parenthesized
REM  code blocks: %WindowsSdkDir% contains "(x86)" and cmd.exe
REM  mis-parses unbalanced parens inside a block.
REM ============================================================
setlocal EnableDelayedExpansion
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :no_vswhere
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH goto :no_vswhere
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 goto :no_vcvars

REM ---- locate cppwinrt projection headers (not on default include path) ----
set "CPPWINRT_INC=%WindowsSdkDir%Include\%WindowsSdkVer%cppwinrt"
if exist "%CPPWINRT_INC%\winrt\base.h" goto :have_cppwinrt
set "CPPWINRT_INC=%WindowsSdkDir%Include\10.0.26100.0\cppwinrt"
if exist "%CPPWINRT_INC%\winrt\base.h" goto :have_cppwinrt
echo [ERROR] cppwinrt headers not found. Install the "C++ /WinRT" SDK component.
exit /b 1
:have_cppwinrt
echo [INFO] cppwinrt: "%CPPWINRT_INC%"
REM pass via INCLUDE env var: putting a path containing "(x86)" on the cl
REM command line inside a FOR block can break cmd.exe paren matching
set "INCLUDE=%CPPWINRT_INC%;%INCLUDE%"

goto :build

:no_vswhere
echo [ERROR] vswhere.exe not found. Install Visual Studio Build Tools 2022.
exit /b 1

:no_vcvars
echo [ERROR] vcvars64 initialization failed.
exit /b 1

:build
set "OUT=build"
set "OBJDIR=%OUT%\obj"
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OBJDIR%" mkdir "%OBJDIR%"

REM /utf-8 : sources contain UTF-8 Chinese string literals
REM /bigobj: C++/WinRT projections exceed the default object section limit
set "CXXFLAGS=/nologo /std:c++17 /EHsc /W3 /O2 /MT /utf-8 /bigobj /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /Isrc /Fo%OBJDIR%\ /c"
set "LDFLAGS=/nologo /SUBSYSTEM:WINDOWS /INCREMENTAL:NO /OUT:%OUT%\PaintingBuddy.exe"
set "LIBS=user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib advapi32.lib d2d1.lib dwrite.lib d3d11.lib dxgi.lib dwmapi.lib shlwapi.lib windowscodecs.lib mf.lib mfplat.lib mfreadwrite.lib mfuuid.lib runtimeobject.lib windowsapp.lib hid.lib winmm.lib comctl32.lib uxtheme.lib propsys.lib"

set "SRCS="
for %%f in (src\*.cpp) do set "SRCS=!SRCS! %%f"

if /i "%~1"=="rebuild" del /q "%OBJDIR%\*.obj" "%OBJDIR%\app.res" 2>nul

REM ---- resource file: embeds res\app.ico into the exe -------------
REM Explorer reads the exe file icon (desktop shortcut, Start menu,
REM taskbar pin, file properties) from the RT_GROUP_ICON resource
REM inside the binary. It never runs the process, so an icon that
REM exists only as a runtime-drawn DIB is invisible to Explorer and a
REM pinned taskbar button degrades to the generic blank icon.
REM
REM /i src : app.rc does #include "resource.h", which sits beside it.
REM rc resolves includes against the current directory plus /i paths,
REM and the current directory here is the project root (see cd /d at
REM the top), so the include dir must be given explicitly. The icon
REM path written inside app.rc is relative to this same root.
REM
REM Deliberately kept out of any parenthesized block: %WindowsSdkDir%
REM contains "(x86)" and cmd.exe mis-parses parens inside a block.
set "RESFILE=%OBJDIR%\app.res"
if not exist "res\app.ico" goto :no_icon
echo.
echo ==== COMPILING RESOURCES ====
echo   src\app.rc
rc /nologo /i src /fo "%RESFILE%" "src\app.rc"
if errorlevel 1 goto :rc_failed

set FAILED=0
echo.
echo ==== COMPILING ====
for %%f in (!SRCS!) do (
    echo   %%f
    cl %CXXFLAGS% "%%f"
    if errorlevel 1 set FAILED=1
)
if "%FAILED%"=="1" goto :compile_failed

echo.
echo ==== LINKING ====
REM app.res is listed explicitly: the glob below only collects *.obj,
REM so the resource would be silently left out of the binary and the
REM exe would lose its icon without any build error at all.
set OBJS=
for %%f in ("%OBJDIR%\*.obj") do set "OBJS=!OBJS! %%f"
link %LDFLAGS% !OBJS! "%RESFILE%" %LIBS%
if errorlevel 1 goto :link_failed

echo.
echo [OK] %CD%\%OUT%\PaintingBuddy.exe
endlocal
exit /b 0

:compile_failed
echo.
echo [FAILED] compile errors; link aborted.
endlocal
exit /b 1

:rc_failed
echo.
echo [FAILED] resource compile (rc.exe) failed; link aborted.
echo          Check src\app.rc and src\resource.h, and make sure
echo          rc.exe is available (Visual Studio Build Tools SDK).
endlocal
exit /b 1

:no_icon
echo.
echo [FAILED] res\app.ico is missing; refusing to build.
echo          Without it the exe would ship with the generic blank icon
echo          in Explorer (desktop / Start menu / taskbar pin), and the
echo          failure would be silent because the sources still compile.
echo          Regenerate the icon per the icon spec, then rebuild.
endlocal
exit /b 1

:link_failed
echo.
echo [FAILED] link errors.
endlocal
exit /b 1
