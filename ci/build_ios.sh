#!/bin/bash
# iPhone のアプリ「計算の受け手」を署名なしで組み、RpcServer.ipa を作る。
# Codemagic から呼ぶ。GitHub Actions の ios.yml と同じ手順。
#   ci/build_ios.sh
#   MINECRAFT=true のときは、マイクラのサーバー Pumpkin も組んで入れる。Pumpkin は GPL-3.0 なので、入れた IPA は人に渡さない
# Xcode は呼ぶ側で選んでおく（26.3）。Foundation Models（Apple Intelligence）は Xcode 26 で組んだときだけ入る
set -euo pipefail
cd "$(dirname "$0")/.."
xcodebuild -version

# Windows 側の llama.cpp と同じ版にそろえる（RPC の取り決めの版が合わないとつながらない）
[ -d llama.cpp ] || git clone --depth 1 --branch b11412 https://github.com/ggml-org/llama.cpp.git llama.cpp
python3 patches/ios_memory.py llama.cpp     # iOS のメモリの上限に合わせて空きを答える
python3 patches/rpc_no_abort.py llama.cpp   # GPU の計算が失敗しても落ちない

# GGML_MAX_NAME=160：stable-diffusion.cpp と同じ長さ。PC 側もそろえる
cmake -S llama.cpp -B build-ios -G Xcode \
    -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 -DGGML_METAL_TARGET_OS=ios \
    -DCMAKE_XCODE_ATTRIBUTE_SUPPORTED_PLATFORMS=iphoneos \
    -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED=NO \
    -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY="" \
    -DBUILD_SHARED_LIBS=OFF -DGGML_METAL=ON -DGGML_METAL_EMBED_LIBRARY=ON \
    -DGGML_RPC=ON -DGGML_RPC_RDMA=OFF -DGGML_NATIVE=OFF -DGGML_OPENMP=OFF -DGGML_BLAS=OFF \
    -DLLAMA_BUILD_TOOLS=OFF -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_TESTS=OFF \
    -DLLAMA_BUILD_SERVER=OFF -DLLAMA_BUILD_COMMON=OFF -DLLAMA_BUILD_APP=OFF -DLLAMA_BUILD_MTMD=OFF \
    -DLLAMA_OPENSSL=OFF -DMTMD_VIDEO=OFF \
    -DCMAKE_C_FLAGS=-DGGML_MAX_NAME=160 -DCMAKE_CXX_FLAGS=-DGGML_MAX_NAME=160
cmake --build build-ios --config Release --target ggml ggml-rpc -- -quiet
mkdir -p libs
find build-ios -name "*.a" -path "*Release-iphoneos*" -exec cp {} libs/ \;
ls -la libs

# Vulkan の受け手は MoltenVK（Vulkan を Metal の上で動かす部品）を使う。iOS 用の静的ライブラリとヘッダー
if [ ! -d MoltenVK ]; then
    curl -sSL -o mvk.tar https://github.com/KhronosGroup/MoltenVK/releases/download/v1.4.2/MoltenVK-ios.tar
    tar xf mvk.tar
fi
ls -la MoltenVK/MoltenVK/static/MoltenVK.xcframework/ios-arm64

EXTRA=()
if [ "${MINECRAFT:-false}" = "true" ]; then
    if [ -f vendor/pumpkin-lib/libpumpkin_ios.a.xz ]; then
        # GitHub Actions の pumpkin-ios で組んだ物を xz で縮めて置いてある（組むと 15 分かかるので、無料枠を使わない）
        xz -dc vendor/pumpkin-lib/libpumpkin_ios.a.xz > libs/libpumpkin_ios.a
        cat vendor/pumpkin-lib/PUMPKIN_COMMIT.txt
    else
        bash ci/build_pumpkin_ios.sh
        cp pumpkin/target/aarch64-apple-ios/release/libpumpkin_ios.a libs/
    fi
    EXTRA=("PUMPKIN_LDFLAGS=-lpumpkin_ios -framework Security -framework SystemConfiguration" "PUMPKIN_SWIFT_FLAG=PUMPKIN")
fi

command -v xcodegen > /dev/null || brew install xcodegen
xcodegen generate

set -o pipefail
# Swift Package（project.yml の packages）を先に取りに行く。取れないときや版が合わないときの誤りはここで出る
xcodebuild -resolvePackageDependencies -project RpcServer.xcodeproj -scheme RpcServer -derivedDataPath dd
# 実際に決まった版（argmax-oss-swift 1.1.1・ml-stable-diffusion 1.1.1 になっているか）
cat RpcServer.xcodeproj/project.xcworkspace/xcshareddata/swiftpm/Package.resolved 2> /dev/null || true
# 記録は build.log に全部残す。tail だけだと最初の誤りが切れるので、失敗したら error の行を先に出す
if ! xcodebuild -project RpcServer.xcodeproj -scheme RpcServer -configuration Release \
    -sdk iphoneos -destination 'generic/platform=iOS' -derivedDataPath dd \
    CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY="" ${EXTRA[@]+"${EXTRA[@]}"} build > build.log 2>&1; then
    grep -n -E "error:|unsafe build flags|cannot be used as a dependency" build.log | head -80 || true
    tail -40 build.log
    exit 1
fi
tail -60 build.log
# Swift Package を入れた分の大きさを段ごとに比べるため
ls -la dd/Build/Products/Release-iphoneos/RpcServer.app/RpcServer
rm -rf Payload RpcServer.ipa
mkdir -p Payload && cp -R dd/Build/Products/Release-iphoneos/RpcServer.app Payload/
# メモリの上限を上げる許可を付けて仮の署名をしておく（AltStore が署名し直すときに引き継ぐ）
codesign -f -s - --entitlements Sources/RpcServer.entitlements Payload/RpcServer.app
codesign -d --entitlements - Payload/RpcServer.app
zip -qry RpcServer.ipa Payload
ls -la RpcServer.ipa
