@echo off
rem PocketGPU for Android: shared part. The other android\*.cmd files call it. Do not run it by itself.
rem Sets ADB, ANDROID_SERIAL, PGPU_PORT, PGPU_CAM_PORT and PGPU_AUDIO_PORT. Exits with code 1 and a message when something is missing.
rem ADB_OK is set once adb.exe is found, so that the caller can still use adb when the phone is missing.
rem The ports on this PC are chosen in this order: the variable (set PGPU_PORT=...) > the port saved in android\port.txt > the default (50300, 50064, 50063).
rem If the PC port is reserved by Windows (see: netsh int ipv4 show excludedportrange protocol=tcp) or used by another program, tools\pgpu_port.ps1
rem   picks a free port outside the reserved ranges (47300, 47400, 47500 and up), prints one line about it, and saves it in android\port.txt.
rem   This happens when the caller sets PGPU_PICK=gpu or PGPU_PICK=gpu,cam,audio before calling this file. To forget the saved ports: delete android\port.txt.
rem Several phones at the same time: set PGPU_FIXED_PORTS=1, ANDROID_SERIAL=the-serial and PGPU_PORT=a-different-port-for-each-phone (PGPU_CAM_PORT and PGPU_AUDIO_PORT too, if the app is used).
rem   Then android\port.txt is neither read nor written, and the ports are never changed behind your back. Every file of this kit uses exactly the numbers you set.
set "ADB_OK="
if defined PGPU_FIXED_PORTS goto fixed_check
:fixed_back
if not defined PGPU_FIXED_PORTS call :load_ports
if "%PGPU_PORT%"=="" set PGPU_PORT=50300
if "%PGPU_CAM_PORT%"=="" set PGPU_CAM_PORT=50064
if "%PGPU_AUDIO_PORT%"=="" set PGPU_AUDIO_PORT=50063

rem ---- find adb.exe. It is not in this kit: get SDK Platform-Tools from Google ----
if defined ADB goto adb_given
for /f "delims=" %%A in ('where adb.exe 2^>nul') do if not defined ADB set "ADB=%%A"
if defined ADB goto adb_ok
for %%D in ("%LOCALAPPDATA%\Android\Sdk\platform-tools" "%USERPROFILE%\platform-tools" "%USERPROFILE%\Downloads\platform-tools" "%USERPROFILE%\Desktop\platform-tools" "C:\platform-tools" "C:\adb" "%ProgramFiles%\platform-tools" "%~dp0..\platform-tools" "%~dp0..\..\platform-tools") do if not defined ADB if exist "%%~D\adb.exe" set "ADB=%%~D\adb.exe"
rem "Extract All" in Windows puts the zip in a folder named after it: platform-tools-latest-windows\platform-tools
for %%D in ("%USERPROFILE%\Downloads\platform-tools-latest-windows\platform-tools" "%USERPROFILE%\Desktop\platform-tools-latest-windows\platform-tools" "%OneDrive%\Desktop\platform-tools" "%OneDrive%\Desktop\platform-tools-latest-windows\platform-tools" "%OneDrive%\Downloads\platform-tools" "%~dp0..\platform-tools-latest-windows\platform-tools" "%~dp0..\..\platform-tools-latest-windows\platform-tools") do if not defined ADB if exist "%%~D\adb.exe" set "ADB=%%~D\adb.exe"
if defined ADB goto adb_ok
echo.
echo ERROR: adb.exe was not found.
echo Download "SDK Platform-Tools for Windows" from Google and unzip it:
echo   https://developer.android.com/tools/releases/platform-tools
echo Then do one of these:
echo   - put the unzipped "platform-tools" folder inside this kit folder, next to it, or at C:\platform-tools
echo   - add the platform-tools folder to PATH
echo   - run   set ADB=C:\path\to\platform-tools\adb.exe   and start this file again from the same window
exit /b 1
:adb_given
if exist "%ADB%" goto adb_ok
echo.
echo ERROR: ADB is set to "%ADB%", but that file does not exist.
exit /b 1
:adb_ok
set ADB_OK=1

rem ---- find the phone ----
"%ADB%" start-server >nul 2>&1
set NDEV=0
set NUNAUTH=0
set ONE=
for /f "skip=1 tokens=1,2" %%A in ('"%ADB%" devices') do call :count "%%A" "%%B"
if %NDEV%==0 goto no_device
if %NDEV% GTR 1 if not defined ANDROID_SERIAL goto many_devices
if not defined ANDROID_SERIAL set "ANDROID_SERIAL=%ONE%"
"%ADB%" get-state 2>nul | findstr /b /c:"device" >nul
if errorlevel 1 goto bad_serial
if not defined PGPU_PICK exit /b 0
goto pick_ports

:count
if "%~2"=="device" set /a NDEV+=1
if "%~2"=="device" set "ONE=%~1"
if "%~2"=="unauthorized" set /a NUNAUTH+=1
goto :eof

:no_device
echo.
if %NUNAUTH% GTR 0 goto unauthorized
echo ERROR: no Android phone found.
echo   - Connect the phone with a USB cable that can carry data (not a charge-only cable).
echo   - Turn on "USB debugging" in Developer options on the phone.
echo   - If the phone asks to allow USB debugging, tap Allow.
echo   - Run   "%ADB%" devices   to see what adb sees.
exit /b 1
:unauthorized
echo ERROR: the phone is connected but not allowed yet.
echo Unlock the phone and tap "Allow" on the "Allow USB debugging?" question. Then run this file again.
exit /b 1
:many_devices
echo.
echo ERROR: more than one device is connected. Unplug the others, or choose one:
"%ADB%" devices
echo   set ANDROID_SERIAL=the-serial-from-the-list-above
exit /b 1
:bad_serial
echo.
echo ERROR: the device "%ANDROID_SERIAL%" is not ready. Run   "%ADB%" devices   to check.
exit /b 1

:pick_ports
rem With PGPU_FIXED_PORTS the numbers are the caller's choice: nothing is checked here and nothing is saved.
if defined PGPU_FIXED_PORTS exit /b 0
rem Check the ports on this PC and save the ones that had to change. The result is read back from android\port.txt.
if exist "%~dp0..\tools\pgpu_port.ps1" "%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\tools\pgpu_port.ps1" -PgpuPick "%PGPU_PICK%" <nul
call :load_ports force
exit /b 0

:fixed_check
rem PGPU_FIXED_PORTS needs the phone and the PC port to be chosen by the caller, so that two phones can never share a port by accident.
if "%ANDROID_SERIAL%"=="" goto fixed_bad
if "%PGPU_PORT%"=="" goto fixed_bad
goto fixed_back
:fixed_bad
echo.
echo ERROR: PGPU_FIXED_PORTS is set, so ANDROID_SERIAL and PGPU_PORT must be set too (a different PGPU_PORT for each phone).
exit /b 1

rem Read android\port.txt (lines like gpu=47300). Without "force" a variable that is already set wins; with "force" the file wins.
:load_ports
if not exist "%~dp0port.txt" exit /b 0
for /f "usebackq tokens=1,* delims==" %%A in ("%~dp0port.txt") do call :load_one "%%A" "%%B" %1
exit /b 0
:load_one
set "LP_KEY=%~1"
set "LP_VAL=%~2"
echo %LP_VAL%| findstr /r /x "[0-9][0-9]*" >nul
if errorlevel 1 exit /b 0
if /i "%LP_KEY%"=="gpu" if "%PGPU_PORT%"=="" set "PGPU_PORT=%LP_VAL%"
if /i "%LP_KEY%"=="gpu" if "%~3"=="force" set "PGPU_PORT=%LP_VAL%"
if /i "%LP_KEY%"=="cam" if "%PGPU_CAM_PORT%"=="" set "PGPU_CAM_PORT=%LP_VAL%"
if /i "%LP_KEY%"=="cam" if "%~3"=="force" set "PGPU_CAM_PORT=%LP_VAL%"
if /i "%LP_KEY%"=="audio" if "%PGPU_AUDIO_PORT%"=="" set "PGPU_AUDIO_PORT=%LP_VAL%"
if /i "%LP_KEY%"=="audio" if "%~3"=="force" set "PGPU_AUDIO_PORT=%LP_VAL%"
set "LP_KEY="
set "LP_VAL="
exit /b 0
