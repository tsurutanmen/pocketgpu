@echo off
rem iPhone を Vulkan の GPU として足して、後ろに書いたアプリを起動する。
rem   iphone-vk.cmd llama-bench.exe -m model.gguf -dev Vulkan1
rem このアプリにだけ効く（レジストリには入れない。ゲームなどほかのアプリには何も起きない）
rem 日本語の Windows の cmd は Shift_JIS で読むので、このファイルも Shift_JIS で保存する
rem iphone_icd.json と iphone_icd.dll は、この cmd と同じ場所に置く
setlocal
set "VK_ADD_DRIVER_FILES=%~dp0iphone_icd.json"
rem Twitch のオーバーレイは iPhone の装置作りを止めるので、このアプリでは挟まない
set DISABLE_TWITCH_VULKAN_OVERLAY=1
rem つなぎ先：USB は usb_forward.py --local 50061 --remote 50060
if "%IPHONE_VK%"=="" set IPHONE_VK=127.0.0.1:50061
%*
