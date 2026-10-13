@echo off
rem PocketGPU for Android: copy the receiver to the phone, start it, and open the USB port to this PC.
rem   android\start_android.cmd
rem Needs adb.exe (Google SDK Platform-Tools). See README_android.txt.
rem Nothing is installed on the phone and no root is needed. The receiver is a file in /data/local/tmp.
rem Only English here on purpose: a Japanese Windows reads .cmd files as Shift_JIS.
setlocal
call "%~dp0_common.cmd"
if errorlevel 1 goto fail

set "SRV="
"%ADB%" shell getprop ro.product.cpu.abi | findstr /b /c:"arm64-v8a" >nul && set "SRV=server_aarch64"
"%ADB%" shell getprop ro.product.cpu.abi | findstr /b /c:"x86_64" >nul && set "SRV=server_x86_64"
if not defined SRV goto bad_abi
if not exist "%~dp0%SRV%" goto no_file

echo Phone:
"%ADB%" shell getprop ro.product.model
echo Receiver: %SRV%  Port: %PGPU_PORT%
echo Stopping an old receiver, if any ...
"%ADB%" shell "kill $(pidof pgpu_server) 2>/dev/null; rm -f /data/local/tmp/pgpu_server; true" >nul 2>&1
echo Copying the receiver to the phone ...
"%ADB%" push "%~dp0%SRV%" /data/local/tmp/pgpu_server >nul
if errorlevel 1 goto push_failed
echo Starting the receiver ...
"%ADB%" shell "cd /data/local/tmp && chmod 755 pgpu_server && (nohup ./pgpu_server 127.0.0.1 %PGPU_PORT% > pgpu_server.log 2>&1 &)"
ping -n 3 127.0.0.1 >nul
"%ADB%" shell pidof pgpu_server | findstr /r "[0-9]" >nul
if errorlevel 1 goto server_failed
"%ADB%" forward tcp:%PGPU_PORT% tcp:%PGPU_PORT% >nul
if errorlevel 1 goto forward_failed

echo.
echo Done. The receiver is running on the phone and port %PGPU_PORT% is connected to this PC.
echo Next: start an app with   pocketgpu_android.cmd "C:\path\to\app.exe"
echo To check that Windows sees the phone GPU: check_android.cmd
echo When you are finished: android\stop_android.cmd
echo Keep the phone unlocked with the screen on while an app is running.
goto end

:bad_abi
echo.
echo ERROR: this phone's CPU is not supported. Only 64-bit ARM (arm64-v8a) phones are supported. This phone says:
"%ADB%" shell getprop ro.product.cpu.abi
goto fail
:no_file
echo.
echo ERROR: %~dp0%SRV% is missing. Unzip the whole kit again.
goto fail
:push_failed
echo.
echo ERROR: could not copy the receiver to the phone (see the message above).
goto fail
:server_failed
echo.
echo ERROR: the receiver did not stay running on the phone. Its log:
"%ADB%" shell cat /data/local/tmp/pgpu_server.log
echo Please send us this log together with the phone model and the Android version.
goto fail
:forward_failed
echo.
echo ERROR: could not open port %PGPU_PORT% on this PC. Another program may be using it.
echo Run android\stop_android.cmd, or use another port:   set PGPU_PORT=50301
goto fail

:fail
if "%PGPU_NOPAUSE%"=="" pause
exit /b 1
:end
if "%PGPU_NOPAUSE%"=="" pause
exit /b 0
