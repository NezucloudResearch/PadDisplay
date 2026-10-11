@echo off
rem Builds the two downloads of a release into host\dist\:
rem   PadDisplay-Setup-<version>.exe      the installer (Inno Setup 6, PadDisplay.iss)
rem   PadDisplay-<version>-portable.zip   the same files without an installer
rem Needs Inno Setup 6 (https://jrsoftware.org/isinfo.php, or: winget install JRSoftware.InnoSetup).
setlocal
cd /d "%~dp0.."
call "%~dp0..\build.cmd" || exit /b 1

rem The version is the one CMake gives the exe: project(PadDisplayHost VERSION x.y.z ...).
set VERSION=
for /f "tokens=3" %%v in ('findstr /b /c:"project(" CMakeLists.txt') do set VERSION=%%v
if not defined VERSION (echo Could not read the version from CMakeLists.txt & exit /b 1)

set ISCC=
for %%p in ("%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" "%ProgramFiles%\Inno Setup 6\ISCC.exe" "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe") do if exist %%p set ISCC=%%p
if not defined ISCC for /f "delims=" %%p in ('where ISCC.exe 2^>nul') do set ISCC="%%p"
if not defined ISCC (echo Inno Setup 6 not found. Install it: winget install JRSoftware.InnoSetup & exit /b 1)

%ISCC% /Qp /DAppVersion=%VERSION% installer\PadDisplay.iss || exit /b 1
echo Installer %CD%\dist\PadDisplay-Setup-%VERSION%.exe

set ZIP=dist\PadDisplay-%VERSION%-portable.zip
if exist "%ZIP%" del "%ZIP%"
rem Windows' own tar (bsdtar) writes a zip for a .zip name. A GNU tar earlier in PATH (Git's) would write a tar file with that name.
"%SystemRoot%\System32\tar.exe" -a -c -f "%ZIP%" -C dist PadDisplay || exit /b 1
echo Portable %CD%\%ZIP%
