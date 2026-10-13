@echo off
rem PocketGPU for Android: shared part. The other android\*.cmd files call it. Do not run it by itself.
rem Sets ADB, ANDROID_SERIAL and PGPU_PORT. Exits with code 1 and a message when something is missing.
rem ADB_OK is set once adb.exe is found, so that the caller can still use adb when the phone is missing.
set "ADB_OK="
if "%PGPU_PORT%"=="" set PGPU_PORT=50300

rem ---- find adb.exe. It is not in this kit: get SDK Platform-Tools from Google ----
if defined ADB goto adb_given
for /f "delims=" %%A in ('where adb.exe 2^>nul') do if not defined ADB set "ADB=%%A"
if defined ADB goto adb_ok
for %%D in ("%LOCALAPPDATA%\Android\Sdk\platform-tools" "%USERPROFILE%\platform-tools" "%USERPROFILE%\Downloads\platform-tools" "%USERPROFILE%\Desktop\platform-tools" "C:\platform-tools" "C:\adb" "%ProgramFiles%\platform-tools" "%~dp0..\platform-tools" "%~dp0..\..\platform-tools") do if not defined ADB if exist "%%~D\adb.exe" set "ADB=%%~D\adb.exe"
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
exit /b 0

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
