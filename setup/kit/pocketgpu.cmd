@echo off
rem PocketGPU: run one app or game on the iPhone GPU.
rem   pocketgpu.cmd "C:\path\to\game.exe"
rem   Steam: put   "C:\path\to\this\folder\pocketgpu.cmd" %command%   in the game's Launch Options.
rem Only the app started from this file sees the iPhone GPU. Nothing is written to the registry.
setlocal
if "%~1"=="" goto pick
set "VK_ADD_DRIVER_FILES=%~dp0iphone_icd.json;%~dp0x86\iphone_icd.json"
if "%IPHONE_VK%"=="" set IPHONE_VK=usb
if "%IPHONE_VK_ONLY%"=="" set IPHONE_VK_ONLY=1
if "%IPHONE_VK_ONLY%"=="1" set "VK_LOADER_DRIVERS_SELECT=*iphone_icd*"
if "%IPHONE_VK_ONLY%"=="1" set "VK_ICD_FILENAMES=%VK_ADD_DRIVER_FILES%"
set DISABLE_TWITCH_VULKAN_OVERLAY=1
set DISABLE_VULKAN_OBS_CAPTURE=1
set DISABLE_VULKAN_OW_OBS_CAPTURE=1
%*
goto :eof
:pick
call "%~dp0Run-on-iPhone-GPU.cmd"
exit /b %errorlevel%
