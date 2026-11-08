@echo off
rem PocketGPU: check that Windows can see the Android phone GPU. Run android\start_android.cmd first.
setlocal
set "VK_ADD_DRIVER_FILES=%~dp0iphone_icd.json;%~dp0x86\iphone_icd.json"
rem The port on this PC: the variable PGPU_PORT, else the port saved in android\port.txt by android\start_android.cmd, else 50300.
if "%PGPU_PORT%"=="" if exist "%~dp0android\port.txt" for /f "usebackq tokens=1,* delims==" %%A in ("%~dp0android\port.txt") do if /i "%%A"=="gpu" echo %%B| findstr /r /x "[0-9][0-9]*" >nul && set "PGPU_PORT=%%B"
if "%PGPU_PORT%"=="" set PGPU_PORT=50300
set IPHONE_VK=127.0.0.1:%PGPU_PORT%
set "VK_ICD_FILENAMES=%VK_ADD_DRIVER_FILES%"
"%~dp0vklist.exe"
echo.
echo If you see a GPU named PocketGPU phone in the list above, it works.
echo If it is not listed, run android\start_android.cmd first.
if "%PGPU_NOPAUSE%"=="" pause
