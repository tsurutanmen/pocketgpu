@echo off
rem PocketGPU for Android: stop the receiver on the phone and close the USB port.
rem   android\stop_android.cmd
rem Only English here on purpose: a Japanese Windows reads .cmd files as Shift_JIS.
setlocal
call "%~dp0_common.cmd"
if errorlevel 1 goto nodev
echo Stopping the receiver on the phone ...
"%ADB%" shell "kill $(pidof pgpu_server) 2>/dev/null; rm -f /data/local/tmp/pgpu_server; true" >nul 2>&1
"%ADB%" forward --remove tcp:%PGPU_PORT% >nul 2>&1
echo Stopped. The receiver is removed from the phone; its log stays at /data/local/tmp/pgpu_server.log.
if "%PGPU_NOPAUSE%"=="" pause
exit /b 0
:nodev
if defined ADB_OK "%ADB%" forward --remove tcp:%PGPU_PORT% >nul 2>&1
echo The receiver on the phone was not stopped. Connect the phone and run this file again, or restart the phone.
if "%PGPU_NOPAUSE%"=="" pause
exit /b 1
