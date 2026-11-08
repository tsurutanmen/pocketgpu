@echo off
rem PocketGPU: set up this PC in one go. It checks what is needed, asks y/N before each install, installs what is missing with winget,
rem shows how to put the app on the iPhone, and ends with the same check as Start-PocketGPU.cmd.
rem   Setup-PocketGPU.cmd               check, ask, install
rem   Setup-PocketGPU.cmd -CheckOnly    only check, change nothing
rem   Setup-PocketGPU.cmd -Yes          answer yes to every question (the UAC prompt and your Apple ID are still yours)
rem Run it WITHOUT "Run as administrator". Things that need administrator rights are asked about once, together (one UAC prompt).
rem Only English here on purpose: a Japanese Windows reads .cmd files as Shift_JIS.
setlocal
if not exist "%~dp0tools\setup.ps1" goto notextracted
if not exist "%~dp0tools\pgpu_common.ps1" goto notextracted
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\setup.ps1" %*
exit /b %errorlevel%
:notextracted
echo.
echo This file was opened from inside the zip, or some files are missing.
echo Right-click the zip file, choose "Extract All...", and open Setup-PocketGPU.cmd in the new folder.
echo.
if "%PGPU_NOPAUSE%"=="" pause
exit /b 1
