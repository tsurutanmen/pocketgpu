@echo off
rem PocketGPU for Android: collect the logs into one zip to send to us.
rem   Send-Android-Log.cmd
rem It checks adb and the phone first (the same check as android\start_android.cmd), then writes a folder and a zip next to this file.
rem No phone number, IMEI, account, PC user name or PC name is in it. Needs adb.exe and USB debugging allowed on the phone.
rem Only English here on purpose: a Japanese Windows reads .cmd files as Shift_JIS.
setlocal
if not exist "%~dp0tools\android-log.ps1" goto notextracted
if not exist "%~dp0tools\pgpu_common.ps1" goto notextracted
if not exist "%~dp0android\_common.cmd" goto notextracted
call "%~dp0android\_common.cmd"
if errorlevel 1 goto stop
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\android-log.ps1"
set "RC=%errorlevel%"
if "%PGPU_NOPAUSE%"=="" pause
exit /b %RC%
:stop
echo.
echo Nothing was collected. Fix the problem above and run Send-Android-Log.cmd again.
if "%PGPU_NOPAUSE%"=="" pause
exit /b 1
:notextracted
echo.
echo This file was opened from inside the zip, or some files are missing.
echo Right-click the zip file, choose "Extract All...", and open Send-Android-Log.cmd in the new folder.
echo.
if "%PGPU_NOPAUSE%"=="" pause
exit /b 1
