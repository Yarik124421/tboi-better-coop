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
pushd "%~dp0.."
if errorlevel 1 exit /b 1
if not exist build\hotfix mkdir build\hotfix
cl /nologo /std:c++17 /O2 /MT /W4 /EHsc /LD /Fo:build\hotfix\ hotfix\hotfix.cpp room_rejoin.cpp /link /OUT:build\bettercoop_hotfix_v11.dll /IMPLIB:build\hotfix\bettercoop_hotfix_v11.lib user32.lib
if errorlevel 1 goto done
cl /nologo /std:c++17 /O2 /MT /W4 /EHsc /Fo:build\hotfix\ hotfix\inject.cpp /link /OUT:build\inject.exe
:done
set "COOP_BUILD_EXIT=%ERRORLEVEL%"
popd
exit /b %COOP_BUILD_EXIT%
