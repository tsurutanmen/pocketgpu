@echo off
rem PocketGPU for Android: measure the USB speed (through adb forward) and write one text file to paste on Discord.
rem   Test-Android-Speed.cmd
rem It checks adb and the phone first (the same check as android\start_android.cmd), then writes a .txt next to this file. It takes about 30 seconds.
rem It shows the USB speed the phone and the PC agree on, MB/s in both directions, and the round trip time in ms.
rem Nothing is installed on the phone and no root is needed. It uses the nc command that Android already has, for a few seconds.
rem No phone number, IMEI, account, serial number, PC user name or PC name is in it. Needs adb.exe and USB debugging allowed on the phone.
rem Only English here on purpose: a Japanese Windows reads .cmd files as Shift_JIS.
setlocal
if not exist "%~dp0tools\android-speed.ps1" goto notextracted
if not exist "%~dp0tools\pgpu_common.ps1" goto notextracted
if not exist "%~dp0tools\pgpu_port.ps1" goto notextracted
if not exist "%~dp0android\_common.cmd" goto notextracted
call "%~dp0android\_common.cmd"
if errorlevel 1 goto stop
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\android-speed.ps1"
set "RC=%errorlevel%"
if "%PGPU_NOPAUSE%"=="" pause
exit /b %RC%
:stop
echo.
echo Nothing was measured. Fix the problem above and run Test-Android-Speed.cmd again.
if "%PGPU_NOPAUSE%"=="" pause
exit /b 1
:notextracted
echo.
echo This file was opened from inside the zip, or some files are missing.
echo Right-click the zip file, choose "Extract All...", and open Test-Android-Speed.cmd in the new folder.
echo.
if "%PGPU_NOPAUSE%"=="" pause
exit /b 1
