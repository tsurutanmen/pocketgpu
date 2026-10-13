@echo off
rem PocketGPU: run one app or game on the Android phone GPU. Run android\start_android.cmd first.
rem   pocketgpu_android.cmd "C:\path\to\game.exe"
rem Only the app started from this file sees the phone GPU. Nothing is written to the registry.
rem DirectX 11 games also need DXVK next to the game. Start-Android-GPU.cmd puts it there for you (it asks first).
rem DXVK_FILTER_DEVICE_NAME=PocketGPU makes DXVK pick the phone GPU ("PocketGPU phone (...)").
setlocal
if "%~1"=="" (
  echo Usage: pocketgpu_android.cmd "C:\path\to\app.exe"
  exit /b 1
)
set "VK_ADD_DRIVER_FILES=%~dp0iphone_icd.json;%~dp0x86\iphone_icd.json"
if "%PGPU_PORT%"=="" set PGPU_PORT=50300
set IPHONE_VK=127.0.0.1:%PGPU_PORT%
if "%DXVK_FILTER_DEVICE_NAME%"=="" if "%IPHONE_VK_NAME%"=="" set DXVK_FILTER_DEVICE_NAME=PocketGPU
if "%IPHONE_VK_ONLY%"=="" set IPHONE_VK_ONLY=1
if "%IPHONE_VK_ONLY%"=="1" set "VK_LOADER_DRIVERS_SELECT=*iphone_icd*"
if "%IPHONE_VK_ONLY%"=="1" set "VK_ICD_FILENAMES=%VK_ADD_DRIVER_FILES%"
set DISABLE_TWITCH_VULKAN_OVERLAY=1
set DISABLE_VULKAN_OBS_CAPTURE=1
set DISABLE_VULKAN_OW_OBS_CAPTURE=1
%*
