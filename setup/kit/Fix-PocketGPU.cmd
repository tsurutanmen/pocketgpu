@echo off
rem PocketGPU: check the same things as Start-PocketGPU.cmd and fix what can be fixed. It asks y/N before each fix and deletes nothing.
rem   Fix-PocketGPU.cmd                      check and fix, one item at a time
rem   Fix-PocketGPU.cmd -CheckOnly           only check, change nothing
rem   Fix-PocketGPU.cmd -Only apple,gpu      only these items: pc files loader apple tools iphone android gpu
rem   Fix-PocketGPU.cmd -KitDir "C:\path"    the kit folder to fix, when this file is not inside the kit
rem No administrator rights are needed. Nothing is written to the registry or to Windows settings.
rem Only English here on purpose: a Japanese Windows reads .cmd files as Shift_JIS.
setlocal
if not exist "%~dp0tools\fix.ps1" goto notextracted
if not exist "%~dp0tools\pgpu_common.ps1" goto notextracted
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\fix.ps1" %*
exit /b %errorlevel%
:notextracted
echo.
echo This file was opened from inside the zip, or some files are missing.
echo Right-click the zip file, choose "Extract All...", and open Fix-PocketGPU.cmd in the new folder.
echo.
if "%PGPU_NOPAUSE%"=="" pause
exit /b 1
