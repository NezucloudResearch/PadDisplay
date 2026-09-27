@echo off
rem Builds build\PadDisplay.exe (Release) with the VS 2022 Build Tools toolchain, then assembles
rem dist\PadDisplay\ - everything a new PC needs: the exe, the bundled virtual monitor driver and the APK.
setlocal
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VS=%%i
if not defined VS (echo Visual Studio Build Tools not found & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build\build.ninja cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build build || exit /b 1
echo Built %~dp0build\PadDisplay.exe

set DIST=%~dp0dist\PadDisplay
if exist "%DIST%" rmdir /s /q "%DIST%"
mkdir "%DIST%" || exit /b 1
copy /y build\PadDisplay.exe "%DIST%\" >nul || exit /b 1
xcopy /e /i /q /y build\driver "%DIST%\driver" >nul || exit /b 1
set APK=%~dp0..\android\app\build\outputs\apk\release\app-release.apk
if exist "%APK%" copy /y "%APK%" "%DIST%\PadDisplay.apk" >nul
for %%f in (LICENSE THIRD_PARTY_NOTICES.md README.md SECURITY.md) do copy /y "%~dp0..\%%f" "%DIST%\" >nul
echo Package %DIST%
