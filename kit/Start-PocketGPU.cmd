@echo off
rem PocketGPU: check this PC step by step and show what is missing. No administrator rights are needed.
rem   Start-PocketGPU.cmd                   check everything in order
rem   Start-PocketGPU.cmd -RemoveShortcuts  remove the Start menu and "Send to" entries made by this kit
rem Nothing is written to the registry or to Windows settings.
rem Only English here on purpose: a Japanese Windows reads .cmd files as Shift_JIS.
setlocal
if not exist "%~dp0iphone_icd.dll" goto notextracted
if not exist "%~dp0tools\start.ps1" goto notextracted
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\start.ps1" %*
exit /b %errorlevel%
:notextracted
echo.
echo This file was opened from inside the zip, or some files are missing.
echo Right-click the zip file, choose "Extract All...", and open Start-PocketGPU.cmd in the new folder.
echo.
if "%PGPU_NOPAUSE%"=="" pause
exit /b 1
