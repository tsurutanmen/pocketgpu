# OpenGL 追加パックの cmd と説明を書く。cmd は Shift_JIS、説明は UTF-8
import os

# opengld・x86 の libgallium_wgl.dll と opengl32.dll は Mesa 26.2.4 の mesa-dist-win（MIT）から入れる
P = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'PocketGPU-OpenGL')

CMD = r'''@echo off
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

set GALLIUM_DRIVER=zink
set IPHONE_VK_DXVK3=1
set IPHONE_VK_ONLY=1
set MESA_LOADER_DRIVER_OVERRIDE=zink
echo iPhone の GPU で OpenGL を動かします。
call "%KIT%iphone-vk.cmd" %*
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
echo "%APPDIR%" に書き込めませんでした。
echo Program Files の中のアプリは、管理者でないとファイルを置けません。
echo アプリのフォルダごと D:\ などに写してから、もう一度やってください。
pause
exit /b 1
'''

PS = r'''param([string]$Path)
# .exe が 32 ビットか 64 ビットかを PE の見出しから読む
try {
  $fs = [IO.File]::OpenRead($Path)
  $br = New-Object IO.BinaryReader($fs)
  $fs.Position = 0x3C
  $fs.Position = $br.ReadInt32() + 4
  $m = $br.ReadUInt16()
  $br.Close()
  if ($m -eq 0x8664) { 'x64' } elseif ($m -eq 0x14c) { 'x86' } else { 'x64' }
} catch { 'x64' }
'''

README = '''PocketGPU OpenGL 追加パック

OpenGL のアプリを iPhone の GPU で動かすための部品です。
zink（Mesa）が OpenGL を Vulkan に直し、PocketGPU のドライバが iPhone に送ります。

【入れ方】
このフォルダの中身を、PocketGPU のキットのフォルダ（iphone-vk.cmd があるフォルダ）に、
そのまま上書きでコピーしてください。

【使い方】
1. いつもどおり PocketGPU をつなぎます（Start-PocketGPU.cmd）。
2. OpenGL のアプリの .exe を、Run-on-iPhone-OpenGL.cmd にドラッグします。
   コマンドプロンプトからは Run-on-iPhone-OpenGL.cmd アプリの.exe でも動きます。

【できること・できないこと】
・OpenGL 2.1 までです。OpenGL 3.0 以上を使うアプリは起動しません。
  3.0 には transform feedback という機能が要りますが、iPhone の MoltenVK にありません。
・「描き方が正しくないことがある」という警告が出ることがありますが、そのまま動きます。
・Program Files の中にあるアプリでは、ファイルを置けないので動きません。
  アプリのフォルダごと D:\\ などに写してから使ってください。

【仕組み】
Windows の OpenGL のアプリは opengl32.dll を直に使うので、そのままでは Vulkan の
ドライバを通りません。この cmd は、アプリの横に zink の opengl32.dll を一時的に置き、
終わったら片づけます。もともと置いてあった物は .pgpubak という名前で取っておきます。
'''

open(os.path.join(P, 'Run-on-iPhone-OpenGL.cmd'), 'w', encoding='cp932', newline='\r\n').write(CMD)
open(os.path.join(P, 'opengl', 'bits.ps1'), 'w', encoding='utf-8-sig', newline='\r\n').write(PS)
open(os.path.join(P, 'README-OpenGL.txt'), 'w', encoding='cp932', newline='\r\n').write(README)
print('書いた')
