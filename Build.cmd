@echo off
setlocal
set "COOP_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%COOP_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "COOP_VS=%%i"
if not defined COOP_VS (
    echo Visual Studio with C++ x86 tools is required.
    exit /b 1
)

call "%COOP_VS%\VC\Auxiliary\Build\vcvarsall.bat" x86
if errorlevel 1 exit /b 1

pushd "%~dp0"
if errorlevel 1 exit /b 1
if not exist build mkdir build
cl /nologo /std:c++17 /O2 /MT /W4 /EHsc /LD /Fo:build\ winmm_loader.cpp room_rejoin.cpp online_checkpoint.cpp /link /DEF:winmm.def /OUT:build\winmm.dll /IMPLIB:build\winmm.lib user32.lib shell32.lib bcrypt.lib
set "COOP_BUILD_EXIT=%ERRORLEVEL%"
popd
exit /b %COOP_BUILD_EXIT%
