@echo off
rem PocketGPU for Android: copy the receiver to the phone, start it, and open the USB port to this PC.
rem   android\start_android.cmd
rem Needs adb.exe (Google SDK Platform-Tools). See README_android.txt.
rem Nothing is installed on the phone and no root is needed. The receiver is a file in /data/local/tmp.
rem Only English here on purpose: a Japanese Windows reads .cmd files as Shift_JIS.
setlocal
rem Check the port on this PC first (Windows can reserve some ports). The chosen port is saved in android\port.txt, and the other files use it.
set "PGPU_PICK=gpu"
call "%~dp0_common.cmd"
if errorlevel 1 goto fail
rem Several phones (PGPU_FIXED_PORTS=1): adb forward quietly takes a port over from another phone. Stop here if this PC port already goes to a different phone.
if defined PGPU_FIXED_PORTS for /f "tokens=1,2" %%A in ('"%ADB%" forward --list') do if "%%B"=="tcp:%PGPU_PORT%" if /i not "%%A"=="%ANDROID_SERIAL%" goto port_taken

set "SRV="
"%ADB%" shell getprop ro.product.cpu.abi | findstr /b /c:"arm64-v8a" >nul && set "SRV=server_aarch64"
"%ADB%" shell getprop ro.product.cpu.abi | findstr /b /c:"x86_64" >nul && set "SRV=server_x86_64"
if not defined SRV goto bad_abi
if not exist "%~dp0%SRV%" goto no_file

echo Phone:
"%ADB%" shell getprop ro.product.model
echo Receiver: %SRV%  Port: %PGPU_PORT%
echo Stopping an old receiver, if any ...
"%ADB%" shell "kill $(pidof pgpu_server) 2>/dev/null; for i in 1 2 3 4 5 6 7 8 9 10; do pidof pgpu_server >/dev/null || break; sleep 0.3; done; kill -9 $(pidof pgpu_server) 2>/dev/null; rm -f /data/local/tmp/pgpu_server /data/local/tmp/pgpu_server.log; true" >nul 2>&1
echo Copying the receiver to the phone ...
"%ADB%" push "%~dp0%SRV%" /data/local/tmp/pgpu_server > "%TEMP%\pocketgpu_push.txt" 2>&1
if errorlevel 1 goto push_failed
echo Starting the receiver ...
"%ADB%" shell "cd /data/local/tmp && chmod 755 pgpu_server && (PGPU_LANG=en nohup ./pgpu_server 127.0.0.1 %PGPU_PORT% > pgpu_server.log 2>&1 &)"
rem Wait until the receiver says it is listening. Starting Vulkan on the phone can take a few seconds.
rem The line is "Vulkan receiver: listening on 127.0.0.1:PORT" (older receivers write it in Japanese, with the same start and address).
set /a TRIES=0
:wait_listen
ping -n 2 127.0.0.1 >nul
set /a TRIES+=1
"%ADB%" shell cat /data/local/tmp/pgpu_server.log 2>nul | findstr /r /c:"^Vulkan .*127\.0\.0\.1:%PGPU_PORT%" >nul
if not errorlevel 1 goto listening
"%ADB%" shell pidof pgpu_server | findstr /r "[0-9]" >nul
if errorlevel 1 goto server_failed
if %TRIES% LSS 30 goto wait_listen
goto server_slow
:listening
"%ADB%" forward tcp:%PGPU_PORT% tcp:%PGPU_PORT% >nul
if errorlevel 1 goto forward_failed
rem Ask Android for the performance game mode for the PocketGPU app (if it is installed). It never stops this file.
call "%~dp0_gamemode.cmd"

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
echo ERROR: could not copy the receiver to the phone. adb said:
type "%TEMP%\pocketgpu_push.txt"
goto fail
:server_failed
echo.
echo ERROR: the receiver did not stay running on the phone. Its log:
"%ADB%" shell cat /data/local/tmp/pgpu_server.log
echo Please send us this log together with the phone model and the Android version.
goto fail
:server_slow
echo.
echo ERROR: the receiver is running on the phone, but it did not start listening within 30 seconds. Its log:
"%ADB%" shell cat /data/local/tmp/pgpu_server.log
echo Unlock the phone and run this file again. If it happens again, send us this log.
goto fail
:port_taken
echo.
echo ERROR: port %PGPU_PORT% on this PC already goes to another phone. Use a different PGPU_PORT for each phone.
"%ADB%" forward --list
goto fail
:forward_failed
echo.
echo ERROR: could not open port %PGPU_PORT% on this PC. Another program may be using it, or Windows reserved it.
echo Run android\stop_android.cmd and try again. This file picks a free port by itself; to choose one:   set PGPU_PORT=47301
echo To see what Windows reserved:   netsh int ipv4 show excludedportrange protocol=tcp
goto fail

:fail
if "%PGPU_NOPAUSE%"=="" pause
exit /b 1
:end
if "%PGPU_NOPAUSE%"=="" pause
exit /b 0
