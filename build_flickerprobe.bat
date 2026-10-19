@echo off
REM Build + run the black-flicker diagnostic probe.
REM Usage: build_flickerprobe.bat "src.mp4" "exported.mp4"
setlocal EnableDelayedExpansion
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH ( echo vswhere: no VC toolset & exit /b 1 )
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo vcvars64 failed & exit /b 1 )

if not exist build\flicker_obj mkdir build\flicker_obj
set "OUT=build\flicker_probe.exe"

cl /nologo /std:c++17 /EHsc /W3 /O2 /MT /utf-8 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   /Isrc /Fobuild\flicker_obj\ ^
   tests\flicker_probe.cpp src\Video.cpp src\Store.cpp src\Config.cpp src\Json.cpp ^
   /Fe"%OUT%" /link /SUBSYSTEM:CONSOLE ole32.lib windowscodecs.lib mf.lib mfplat.lib mfreadwrite.lib mfuuid.lib shlwapi.lib shell32.lib
if errorlevel 1 ( echo COMPILE FAILED & exit /b 1 )

echo ==== RUN flicker_probe ====
"%OUT%" %1 %2
exit /b %ERRORLEVEL%
