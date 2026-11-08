@echo off
rem OpenGL のアプリを iPhone の GPU で動かす。zink が OpenGL を Vulkan に直す
rem   Run-on-iPhone-OpenGL.cmd アプリの.exe [引数...]
rem   または アプリの .exe をこのファイルにドラッグする
rem 日本語の Windows の cmd は Shift_JIS で読むので、このファイルは Shift_JIS で保存する
setlocal enabledelayedexpansion
if "%~1"=="" (
  echo 使い方: Run-on-iPhone-OpenGL.cmd アプリの.exe
  echo アプリの .exe をこのファイルにドラッグしても動きます。
  pause
  exit /b 1
)
set "KIT=%~dp0"
set "APP=%~f1"
set "APPDIR=%~dp1"
if not exist "%APP%" (
  echo "%APP%" が見つかりません。
  pause
  exit /b 1
)
rem 32 ビットか 64 ビットかを見る
set "BITS=x64"
for /f %%b in ('powershell -NoProfile -ExecutionPolicy Bypass -File "%KIT%opengl\bits.ps1" -Path "%APP%"') do set "BITS=%%b"
echo %~nx1 は %BITS% です。

set "SRC=%KIT%opengl\%BITS%"
if not exist "%SRC%\opengl32.dll" (
  echo %SRC% に opengl32.dll がありません。
  pause
  exit /b 1
)
rem アプリの横に zink の部品を置く。もともと有ったものは .pgpubak に取っておく
set "PUT=0"
if not exist "%APPDIR%opengl32.dll" goto copy
if exist "%APPDIR%opengl32.dll.pgpubak" goto copy
move /y "%APPDIR%opengl32.dll" "%APPDIR%opengl32.dll.pgpubak" >nul
if exist "%APPDIR%libgallium_wgl.dll" move /y "%APPDIR%libgallium_wgl.dll" "%APPDIR%libgallium_wgl.dll.pgpubak" >nul
:copy
copy /y "%SRC%\opengl32.dll" "%APPDIR%" >nul || goto nowrite
copy /y "%SRC%\libgallium_wgl.dll" "%APPDIR%" >nul || goto nowrite
set "PUT=1"

:run
rem 配るキットは pocketgpu.cmd、開発用の置き方は iphone-vk.cmd
set "LAUNCH=%KIT%pocketgpu.cmd"
if not exist "%LAUNCH%" set "LAUNCH=%KIT%iphone-vk.cmd"
if not exist "%LAUNCH%" goto nokit

rem Java のアプリ（Minecraft など）は LWJGL が opengl32.dll を自分で読むので、場所を JVM に渡す。
rem ランチャーが別の場所から起こす javaw.exe にも、この設定は引き継がれる
set "JAVA_TOOL_OPTIONS=-Dorg.lwjgl.opengl.libname=%KIT%opengl\x64\opengl32.dll %JAVA_TOOL_OPTIONS%"
set "PATH=%KIT%opengl\x64;%PATH%"
set GALLIUM_DRIVER=zink
set IPHONE_VK_DXVK3=1
set IPHONE_VK_ONLY=1
set MESA_LOADER_DRIVER_OVERRIDE=zink
echo iPhone の GPU で OpenGL を動かします。
call "%LAUNCH%" %*
set "RC=%errorlevel%"

rem 置いた部品を片づける
if "%PUT%"=="1" (
  del /q "%APPDIR%opengl32.dll" 2>nul
  del /q "%APPDIR%libgallium_wgl.dll" 2>nul
  if exist "%APPDIR%opengl32.dll.pgpubak" move /y "%APPDIR%opengl32.dll.pgpubak" "%APPDIR%opengl32.dll" >nul
  if exist "%APPDIR%libgallium_wgl.dll.pgpubak" move /y "%APPDIR%libgallium_wgl.dll.pgpubak" "%APPDIR%libgallium_wgl.dll" >nul
)
exit /b %RC%

:nowrite
echo.
echo "%APPDIR%" に部品を置けませんでした（Program Files や Microsoft Store のアプリ）。
echo Minecraft のような Java のアプリは、このまま iPhone で動きます。
echo ほかのアプリは、フォルダごと D:\ などに写してから、もう一度やってください。
echo.
set "PUT=0"
goto run

:nokit
echo.
echo このファイルは、PocketGPU のキットのフォルダに置いてください。
echo pocketgpu.cmd と同じ場所に置く必要があります。
pause
exit /b 1
