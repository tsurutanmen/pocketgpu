@echo off
rem Test double for vklist.exe. It always lists a PC GPU. It lists the "iPhone GPU" only when the driver would load:
rem VK_ICD_FILENAMES (or VK_ADD_DRIVER_FILES) points to a json that has iphone_icd.dll next to it.
echo GPU0: NVIDIA GeForce Test  (vendor 0x10de, type 2, Vulkan 1.3, 12288 MiB)
set "LIST=%VK_ICD_FILENAMES%"
if "%LIST%"=="" goto :eof
for /f "tokens=1 delims=;" %%A in ("%LIST%") do set "J=%%~A"
for %%B in ("%J%") do if exist "%%~dpBiphone_icd.dll" echo GPU1: Apple A19 Pro GPU (fake)  (vendor 0x106b, type 1, Vulkan 1.2, 8192 MiB)
exit /b 0
