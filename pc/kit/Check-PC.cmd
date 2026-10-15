@echo off
rem PocketGPU: check this PC and write one text file to paste on Discord.
rem   Check-PC.cmd
rem It only reads. It changes nothing on this PC and needs no administrator rights.
rem It reads: the Windows version, Smart App Control, the kit files and their download marks, Windows security records that mention kit files (last 7 days),
rem the Vulkan driver entries, the GPU names, the Windows graphics settings for kit files, Apple Mobile Device Service, how many iPhones USB sees, and the USB power settings.
rem No user name, PC name, serial number, e-mail address, or iPhone name or UDID is in the text file. It is written next to this file.
rem Only English here on purpose: a Japanese Windows reads .cmd files as Shift_JIS.
setlocal
if not exist "%~dp0tools\pc-check.ps1" goto notextracted
if not exist "%~dp0tools\pgpu_common.ps1" goto notextracted
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\pc-check.ps1"
set "RC=%errorlevel%"
if "%PGPU_NOPAUSE%"=="" pause
exit /b %RC%
:notextracted
echo.
echo This file was opened from inside the zip, or some files are missing.
echo Right-click the zip file, choose "Extract All...", and open Check-PC.cmd in the new folder.
echo.
if "%PGPU_NOPAUSE%"=="" pause
exit /b 1
