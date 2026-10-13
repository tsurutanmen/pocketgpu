@echo off
rem PocketGPU: check that Windows can see the Android phone GPU. Run android\start_android.cmd first.
setlocal
set "VK_ADD_DRIVER_FILES=%~dp0iphone_icd.json;%~dp0x86\iphone_icd.json"
if "%PGPU_PORT%"=="" set PGPU_PORT=50300
set IPHONE_VK=127.0.0.1:%PGPU_PORT%
set "VK_ICD_FILENAMES=%VK_ADD_DRIVER_FILES%"
"%~dp0vklist.exe"
echo.
echo If you see a GPU named PocketGPU phone in the list above, it works.
echo If it is not listed, run android\start_android.cmd first.
if "%PGPU_NOPAUSE%"=="" pause
