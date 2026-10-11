#!/bin/bash
# 古い iPhone（iOS 15・16。iPhone 8 など）用の軽いアプリ PocketCam を署名なしで組み、PocketCam.ipa を作る。
# Codemagic の ios-lite から呼ぶ。本体の ci/build_ios.sh とは別の台本で、llama.cpp・MoltenVK・Pumpkin は組まない。
#   ci/build_lite.sh
# 中身はウェブカメラ（CameraBridge.swift）とスピーカーとマイク（AudioBridge.swift）だけ。project.yml の PocketCam のターゲット
# Xcode は呼ぶ側で選んでおく（26.3。iOS 15 まで下げて組めるのは Apple の資料で Xcode 26 の範囲の下限が iOS 15 のため）
set -euo pipefail
cd "$(dirname "$0")/.."
xcodebuild -version

command -v xcodegen > /dev/null || brew install xcodegen
xcodegen generate

# 本体の Swift Package（argmax-oss-swift・ml-stable-diffusion）は project.yml の packages にあるので取りに行くが、
# PocketCam のターゲットは頼らないので、組むことも入れることもない
xcodebuild -list -project RpcServer.xcodeproj
# 記録は build_lite.log に全部残す。失敗したら error の行を先に出す
if ! xcodebuild -project RpcServer.xcodeproj -scheme PocketCam -configuration Release \
    -sdk iphoneos -destination 'generic/platform=iOS' -derivedDataPath dd \
    CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY="" build > build_lite.log 2>&1; then
    grep -n -E "error:|warning: .*(deprecated|unavailable|introduced)" build_lite.log | head -80 || true
    tail -40 build_lite.log
    exit 1
fi
tail -30 build_lite.log
# iOS 15・16 で使えない API を使っていると、ここに出る（組めても警告になる物）
grep -n -E "is only available in iOS 1[6-9]|is only available in iOS 2[0-9]|was deprecated in iOS" build_lite.log | head -40 || true

APP=dd/Build/Products/Release-iphoneos/PocketCam.app
ls -la "$APP" "$APP/PocketCam"
# 最低の iOS が 15.0 か・バンドル ID・版
MIN_OS=$(/usr/libexec/PlistBuddy -c "Print :MinimumOSVersion" "$APP/Info.plist")
echo "MinimumOSVersion=$MIN_OS"
plutil -p "$APP/Info.plist" | grep -E "CFBundleIdentifier|CFBundleShortVersionString|CFBundleVersion|NSCameraUsageDescription|NSMicrophoneUsageDescription|NSLocalNetworkUsageDescription|UIBackgroundModes" || true
[ "$MIN_OS" = "15.0" ] || { echo "MinimumOSVersion が 15.0 になっていない"; exit 1; }
# 権限の文の日英（InfoPlist.strings）が入ったか。無くても動く（Info.plist の日本語の文が出る）ので止めない
ls -la "$APP/ja.lproj" "$APP/en.lproj" || echo "InfoPlist.strings の ja.lproj か en.lproj が入っていない"
# GPU の部品と AI のパッケージが入っていないか（MoltenVK・ggml・Pumpkin・WhisperKit・TTSKit・StableDiffusion）
if otool -L "$APP/PocketCam" | grep -i -E "moltenvk|ggml|pumpkin|whisperkit|ttskit|stablediffusion"; then
    echo "PocketCam に GPU か AI の部品が入っている"
    exit 1
fi
if [ -d "$APP/Frameworks" ]; then ls -la "$APP/Frameworks"; fi
otool -L "$APP/PocketCam"

rm -rf Payload PocketCam.ipa
mkdir -p Payload && cp -R "$APP" Payload/
# 仮の署名だけしておく（特別な許可は付けない。AltStore が署名し直す）
codesign -f -s - Payload/PocketCam.app
zip -qry PocketCam.ipa Payload
ls -la PocketCam.ipa
