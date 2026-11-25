@echo off
setlocal
set "GAME_EXE=Scarface"
cd /d "%~dp0"
set "OUT=out\build\release"
set "STAGE=%OUT%\release"
rem the release archive name; pass one as the first argument to override
set "ARCHIVE=%GAME_EXE%.zip"
if not "%~1"=="" set "ARCHIVE=%~1"
for %%i in ("%ARCHIVE%") do set "ARCHIVE=%%~fi"
rem the Visual C++ runtime beside the executable, so a fresh Windows install needs nothing more
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
for /d %%v in ("%VSDIR%\VC\Redist\MSVC\1*") do for /d %%c in ("%%v\x64\Microsoft.VC*.CRT") do set "VCRUNTIME=%%c"
if not exist "%VCRUNTIME%\vcruntime140.dll" (echo Visual C++ runtime not found: install Visual Studio with the C++ workload & exit /b 1)
if not exist "%OUT%\%GAME_EXE%.exe" call build.bat || exit /b 1
rem the configure step writes where the SDK is
if not exist "%OUT%\sdk_dir.txt" cmake --preset release >nul || exit /b 1
if not exist "%OUT%\sdk_dir.txt" echo %OUT%\sdk_dir.txt was not written, update the SDK. && exit /b 1
set /p SDK=<%OUT%\sdk_dir.txt

rem stage everything the archive holds, so nothing extra (work files, logs, settings) can slip in
del /q "%ARCHIVE%" 2>nul
rd /s /q "%STAGE%" 2>nul
mkdir "%STAGE%\assets" "%STAGE%\docs"
copy /y "%OUT%\%GAME_EXE%.exe" "%STAGE%\" >nul
copy /y README.md "%STAGE%\" >nul
copy /y recomplauncher.json "%STAGE%\" >nul
copy /y docs\controls.md "%STAGE%\docs\" >nul
copy /y docs\debugging.md "%STAGE%\docs\" >nul
copy /y docs\white-ui-panels.md "%STAGE%\docs\" >nul
copy /y docs\experimental-fps.md "%STAGE%\docs\" >nul
copy /y docs\hud-glyphs-hotkeys.md "%STAGE%\docs\" >nul
copy /y docs\PROJECT-NOTES.md "%STAGE%\docs\" >nul
copy /y "%SDK%\THIRD_PARTY.txt" "%STAGE%\" >nul
for %%f in (msvcp140 vcruntime140 vcruntime140_1) do copy /y "%VCRUNTIME%\%%f.dll" "%STAGE%\" >nul
rem the bundled compiler without its work folder: no game code goes in the archive, the first start builds it
robocopy "%OUT%\compiler" "%STAGE%\compiler" /e /xd work /nfl /ndl /njh /njs >nul
if errorlevel 8 (echo Could not stage the compiler. & exit /b 1)
rem xbe_recomp.exe needs its runtime beside itself
for %%f in (msvcp140 vcruntime140) do copy /y "%VCRUNTIME%\%%f.dll" "%STAGE%\compiler\" >nul

rem Windows 10 and later ship bsdtar, which writes zip archives when given a .zip name
where tar >nul 2>nul || (echo tar.exe was not found: Windows 10 or later is required to package a release. & exit /b 1)
pushd "%STAGE%"
tar -a -c -f "%ARCHIVE%" * || (popd & exit /b 1)
popd
echo Wrote %ARCHIVE%
