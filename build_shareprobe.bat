@echo off
setlocal EnableDelayedExpansion
cd /d "%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if not exist build mkdir build
echo ==== BUILD share_probe ====
cl /nologo /std:c++17 /EHsc /W3 /O2 /MT /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /Fobuild\share_probe.obj /Febuild\share_probe.exe tests\share_probe.cpp /link user32.lib
if errorlevel 1 (echo BUILD_FAILED & exit /b 1)
echo ==== RUN share_probe ====
build\share_probe.exe
set RC=%ERRORLEVEL%
endlocal
exit /b %RC%
