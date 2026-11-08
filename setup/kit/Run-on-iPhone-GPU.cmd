@echo off
rem PocketGPU: run one game or app on the iPhone GPU.
rem   Drop the game's .exe (or its shortcut) onto this file, or double-click this file to choose the .exe.
rem   Run-on-iPhone-GPU.cmd "C:\Games\game.exe" [arguments for the game]
rem Only the program started from here sees the iPhone GPU. Nothing is written to the registry.
rem Only English here on purpose: a Japanese Windows reads .cmd files as Shift_JIS.
setlocal
if not exist "%~dp0tools\run.ps1" goto notextracted
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\run.ps1" %*
exit /b %errorlevel%
:notextracted
echo.
echo This file was opened from inside the zip, or some files are missing.
echo Right-click the zip file, choose "Extract All...", and use the files in the new folder.
echo.
if "%PGPU_NOPAUSE%"=="" pause
exit /b 1
