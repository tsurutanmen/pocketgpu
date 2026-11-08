@echo off
rem Build PocketGPU.exe with the C# compiler that ships with Windows (.NET Framework 4.8). No SDK, no download.
rem   build.cmd     ->  out\PocketGPU.exe  (one file)
rem The XAML files in src\ui are embedded into the exe as resources named ui.NAME.xaml.
rem To try XAML edits without rebuilding: set PGPU_XAML_DIR=%~dp0src\ui before starting the exe.
rem The tools (webcam_rx.exe, camera_setup.exe, ipaudio.exe, livecap.exe) are found next to the exe, in tools\ or extras\,
rem or through tools.ini next to the exe (see src\Tools.cs).
rem Only English here on purpose: a Japanese Windows reads .cmd files as Shift_JIS.
setlocal EnableDelayedExpansion
cd /d "%~dp0"
set "FW=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319"
set "CSC=%FW%\csc.exe"
if not exist "%CSC%" (
  echo csc.exe was not found: %CSC%
  exit /b 1
)
if not exist out mkdir out
set "RES="
for %%f in (src\ui\*.xaml) do set "RES=!RES! /resource:%%f,ui.%%~nxf"
set "REFS=/r:"%FW%\WPF\PresentationFramework.dll" /r:"%FW%\WPF\PresentationCore.dll" /r:"%FW%\WPF\WindowsBase.dll" /r:"%FW%\System.Xaml.dll" /r:"%FW%\System.Web.Extensions.dll" /r:"%FW%\System.Management.dll" /r:"%FW%\System.ServiceProcess.dll" /r:"%FW%\System.Drawing.dll" /r:"%FW%\System.Windows.Forms.dll" /r:System.dll /r:System.Core.dll /r:System.Xml.dll"
set "OPTS=/nologo /target:winexe /platform:anycpu /optimize+ /unsafe /codepage:65001 /utf8output /win32manifest:src\app.manifest"

set "ICON="
if exist src\app.ico set "ICON=/win32icon:src\app.ico"
"%CSC%" %OPTS% %ICON% /out:out\PocketGPU.exe %REFS% %RES% src\*.cs
if errorlevel 1 exit /b 1

rem First build only: draw the icon with the app itself, then build again with it
if not exist src\app.ico (
  start "" /wait out\PocketGPU.exe --make-icon src\app.ico
  if not exist src\app.ico (
    echo Could not make src\app.ico
    exit /b 1
  )
  "%CSC%" %OPTS% /win32icon:src\app.ico /out:out\PocketGPU.exe %REFS% %RES% src\*.cs
  if errorlevel 1 exit /b 1
)
echo Built out\PocketGPU.exe
exit /b 0
