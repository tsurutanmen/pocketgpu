@echo off
rem PocketGPU: update this kit. Downloads only the files that changed, checks each one with SHA-256,
rem stops the PocketGPU tools that are running, keeps the old files in update-backup\ and puts them back if anything fails.
rem   Update-PocketGPU.cmd           check and update
rem   Update-PocketGPU.cmd -Check    only check, change nothing
rem   Update-PocketGPU.cmd -NoIpa    do not download the new iPhone app (.ipa)
rem No administrator rights are needed. Nothing is written to the registry.
rem Only English here on purpose: a Japanese Windows reads .cmd files as Shift_JIS.
rem The PowerShell line and the exit are one block on purpose: the update may replace this file while it runs,
rem and cmd.exe reads a block in one go, so it does not read the new file halfway.
rem "call exit /b %%errorlevel%%" reads the exit code after PowerShell ends. A plain "exit /b" in a block returns 0 to "cmd /c".
rem Exit codes: 0 done or up to date, 10 a new version is available (with -Check), 1 failed, 2 no update information, 3 not valid.
setlocal
if not exist "%~dp0tools\update.ps1" goto notextracted
(
  "%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\update.ps1" %*
  call exit /b %%errorlevel%%
)
:notextracted
echo.
echo This file was opened from inside the zip, or some files are missing.
echo Right-click the zip file, choose "Extract All...", and open Update-PocketGPU.cmd in the new folder.
echo.
if "%PGPU_NOPAUSE%"=="" pause
exit /b 1
