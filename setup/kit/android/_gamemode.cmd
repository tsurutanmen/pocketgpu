@echo off
rem PocketGPU for Android: ask Android for the "performance" game mode for the PocketGPU app. start_android.cmd and android_app.cmd call this after they set ADB.
rem Always exits with code 0: when it fails (Android 11 or older has no "cmd game", the app is not installed, the phone refuses) it only says so.
rem   To skip it:   set PGPU_NOGAMEMODE=1
rem The package name is com.tsurutanmen.pocketgpu (the app's AndroidManifest.xml). The app declares itself a game and lists the performance mode in res/xml/game_mode_config.xml.
rem Only English here on purpose: a Japanese Windows reads .cmd files as Shift_JIS.
setlocal
if "%PGPU_NOGAMEMODE%"=="1" exit /b 0
if not defined ADB exit /b 0
set "GM_PKG=com.tsurutanmen.pocketgpu"
"%ADB%" shell pm path %GM_PKG% 2>nul | findstr /b /c:"package:" >nul
if errorlevel 1 goto gm_noapp
set "GM_OUT=%TEMP%\pocketgpu_gamemode.txt"
"%ADB%" shell cmd game mode performance %GM_PKG% > "%GM_OUT%" 2>&1 <nul
if errorlevel 1 goto gm_second
findstr /i /c:"exception" /c:"error" /c:"unknown" /c:"usage" /c:"can't find" /c:"not found" /c:"inaccessible" /c:"fail" "%GM_OUT%" >nul
if errorlevel 1 goto gm_ok
:gm_second
"%ADB%" shell cmd game set --mode performance %GM_PKG% > "%GM_OUT%" 2>&1 <nul
if errorlevel 1 goto gm_failed
findstr /i /c:"exception" /c:"error" /c:"unknown" /c:"usage" /c:"can't find" /c:"not found" /c:"inaccessible" /c:"fail" "%GM_OUT%" >nul
if errorlevel 1 goto gm_ok
:gm_failed
echo Game mode: not set. adb said:
type "%GM_OUT%"
exit /b 0
:gm_noapp
echo Game mode: not set, the PocketGPU app is not installed
exit /b 0
:gm_ok
echo Game mode: performance requested for %GM_PKG%
exit /b 0
