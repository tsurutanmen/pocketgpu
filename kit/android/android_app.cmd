@echo off
rem PocketGPU for Android (app version): open the USB ports to the PocketGPU app and turn its features on.
rem   android\android_app.cmd          start: forward the ports and turn on webcam, speaker and mic, and GPU
rem   android\android_app.cmd stop     close the ports and turn the features off
rem Needs adb.exe and USB debugging. Install the app first:  adb install android\pocketgpu.apk
rem Only English here on purpose: a Japanese Windows reads .cmd files as Shift_JIS.
rem
rem Port form for the webcam (50064) and the audio (50063). It depends on the app version:
rem   app 0.1.2 and newer:   adb forward tcp:50064 localabstract:pocketgpu-webcam   and   adb forward tcp:50063 localabstract:pocketgpu-audio
rem                          The app accepts these only from adb, so other apps on the phone cannot read the camera or the mic.
rem   app 0.1.1 and older:   adb forward tcp:50064 tcp:50064   and   adb forward tcp:50063 tcp:50063
rem This file reads the app version from the phone and uses the matching form.
rem   If the version cannot be read it uses the new form. If adb refuses the new form it uses the old one.
rem   To choose the form yourself:   set PGPU_APP_FORWARD=new   or   set PGPU_APP_FORWARD=old
rem   The 0.1.2 app also takes the old form when "Also accept old PC tools" is on in the app settings. It is off by default.
rem Other ports:   set PGPU_CAM_PORT=50064   set PGPU_AUDIO_PORT=50063   set PGPU_PORT=50300
rem GPU: nothing changed. If the kit receiver from android\start_android.cmd is running on the phone, the app does not
rem   start its own receiver and the kit receiver is used. Otherwise port %PGPU_PORT% goes to the receiver inside the app (port 50060).
setlocal
call "%~dp0_common.cmd"
if errorlevel 1 goto nodev
set "PKG=com.tsurutanmen.pocketgpu"
set "APP=%PKG%/.MainActivity"
if "%PGPU_CAM_PORT%"=="" set PGPU_CAM_PORT=50064
if "%PGPU_AUDIO_PORT%"=="" set PGPU_AUDIO_PORT=50063
if /i "%~1"=="stop" goto stop

"%ADB%" shell pm path %PKG% | findstr /b /c:"package:" >nul
if errorlevel 1 goto no_app

rem ---- which form of the ports does this app version take? ----
set "APPVER="
for /f "tokens=1,2 delims== " %%U in ('"%ADB%" shell dumpsys package %PKG%') do if "%%U"=="versionName" if not defined APPVER set "APPVER=%%V"
set "FORM=new"
set "FORMBY=the app version could not be read"
set "V1="
set "V2="
set "V3="
if defined APPVER for /f "tokens=1-3 delims=.-+_ " %%A in ("%APPVER%") do set "V1=%%A" & set "V2=%%B" & set "V3=%%C"
if not defined V1 goto ver_done
if not defined V2 set "V2=0"
if not defined V3 set "V3=0"
echo %V1%.%V2%.%V3%| findstr /r /x "[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*" >nul
if errorlevel 1 goto ver_done
set /a VN=1000000*%V1%+1000*%V2%+%V3%
set "FORMBY=app version %APPVER%"
if %VN% LSS 1002 set "FORM=old"
:ver_done
set "FORCED="
if /i "%PGPU_APP_FORWARD%"=="new" set "FORM=new"
if /i "%PGPU_APP_FORWARD%"=="new" set "FORCED=1"
if /i "%PGPU_APP_FORWARD%"=="old" set "FORM=old"
if /i "%PGPU_APP_FORWARD%"=="old" set "FORCED=1"
if defined FORCED set "FORMBY=PGPU_APP_FORWARD=%PGPU_APP_FORWARD%"

if "%FORM%"=="old" goto fwd_old
"%ADB%" forward tcp:%PGPU_CAM_PORT% localabstract:pocketgpu-webcam >nul 2>&1
if errorlevel 1 goto fwd_new_failed
"%ADB%" forward tcp:%PGPU_AUDIO_PORT% localabstract:pocketgpu-audio >nul 2>&1
if errorlevel 1 goto fwd_new_failed
goto fwd_done
:fwd_new_failed
if defined FORCED goto forward_failed
echo adb did not accept the new port form. Trying the old form ...
echo The 0.1.2 app takes the old form only when "Also accept old PC tools" is on in the app settings.
set "FORM=old"
set "FORMBY=adb refused the new form"
:fwd_old
"%ADB%" forward tcp:%PGPU_CAM_PORT% tcp:50064 >nul || goto forward_failed
"%ADB%" forward tcp:%PGPU_AUDIO_PORT% tcp:50063 >nul || goto forward_failed
:fwd_done

set "GPU_TARGET=tcp:50060"
set "GPU_WHO=the app receiver"
"%ADB%" shell pidof pgpu_server | findstr /r "[0-9]" >nul
if errorlevel 1 goto gpu_forward
set "GPU_TARGET=tcp:%PGPU_PORT%"
set "GPU_WHO=the kit receiver started by android\start_android.cmd"
:gpu_forward
"%ADB%" forward tcp:%PGPU_PORT% %GPU_TARGET% >nul || goto forward_failed
"%ADB%" shell am start -n %APP% --ez webcam true --ez audio true --ez gpu true >nul
echo.
echo Done. If the phone asks for camera or microphone access, tap Allow.
echo Port form: %FORM% (%FORMBY%)
echo   Webcam:          webcam_rx.exe --addr 127.0.0.1:%PGPU_CAM_PORT%
echo   Speaker and mic: ipaudio.exe --connect 127.0.0.1:%PGPU_AUDIO_PORT% --speaker --mic
echo   GPU:             pocketgpu_android.cmd "C:\path\to\app.exe"   (uses %GPU_WHO%)
if "%FORM%"=="old" echo The app on the phone is old. The new app is android\pocketgpu.apk. To update:   "%ADB%" install -r "%~dp0pocketgpu.apk"
if "%FORM%"=="old" if not defined FORCED echo The new app takes the camera and the mic only from adb, so other apps on the phone cannot read them.
echo When you are finished: android\android_app.cmd stop
goto end

:stop
"%ADB%" shell am start -n %APP% --ez webcam false --ez audio false --ez gpu false >nul 2>&1
"%ADB%" forward --remove tcp:%PGPU_CAM_PORT% >nul 2>&1
"%ADB%" forward --remove tcp:%PGPU_AUDIO_PORT% >nul 2>&1
"%ADB%" forward --remove tcp:%PGPU_PORT% >nul 2>&1
echo Stopped.
goto end

:nodev
if /i not "%~1"=="stop" goto fail
if not defined ADB_OK goto fail
if "%PGPU_CAM_PORT%"=="" set PGPU_CAM_PORT=50064
if "%PGPU_AUDIO_PORT%"=="" set PGPU_AUDIO_PORT=50063
"%ADB%" forward --remove tcp:%PGPU_CAM_PORT% >nul 2>&1
"%ADB%" forward --remove tcp:%PGPU_AUDIO_PORT% >nul 2>&1
"%ADB%" forward --remove tcp:%PGPU_PORT% >nul 2>&1
echo The phone was not found. The ports on this PC were closed, but the app on the phone was not told to stop.
goto fail

:no_app
echo.
echo ERROR: the PocketGPU app is not installed on the phone. Install it with:   "%ADB%" install "%~dp0pocketgpu.apk"
goto fail
:forward_failed
echo.
echo ERROR: could not open a port on this PC. Another program may be using %PGPU_CAM_PORT%, %PGPU_AUDIO_PORT% or %PGPU_PORT%.
echo Run android\android_app.cmd stop, or use other ports (see the top of this file).
goto fail

:fail
if "%PGPU_NOPAUSE%"=="" pause
exit /b 1
:end
if "%PGPU_NOPAUSE%"=="" pause
exit /b 0
