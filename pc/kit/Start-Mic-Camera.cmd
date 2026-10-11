@echo off
rem PocketGPU: use the iPhone only as a microphone and a webcam. The iPhone GPU is not used. No administrator rights are needed.
rem   Start-Mic-Camera.cmd                          start the camera and the microphone
rem   Start-Mic-Camera.cmd -List                    show the iPhones connected over USB
rem   Start-Mic-Camera.cmd -Device 00008110-0012    choose the iPhone (UDID, or its first 6+ characters) when several are connected
rem   Start-Mic-Camera.cmd -NoMic | -NoCamera | -Speaker | -Mic "CABLE Input"
rem   Start-Mic-Camera.cmd -Camera front|back|ultrawide|tele  -Quality standard|high|max
rem The only registry change is the camera for the current user (HKCU), and only after asking.
rem Only English here on purpose: a Japanese Windows reads .cmd files as Shift_JIS.
setlocal
if not exist "%~dp0tools\miccam.ps1" goto notextracted
if not exist "%~dp0tools\pgpu_common.ps1" goto notextracted
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\miccam.ps1" %*
exit /b %errorlevel%
:notextracted
echo.
echo This file was opened from inside the zip, or some files are missing.
echo Right-click the zip file, choose "Extract All...", and open Start-Mic-Camera.cmd in the new folder.
echo.
if "%PGPU_NOPAUSE%"=="" pause
exit /b 1
