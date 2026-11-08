@echo off
rem PocketGPU: write pocketgpu-report.txt about this PC (no user name, PC name or IP).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0report.ps1" %*
pause
