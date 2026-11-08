#!/bin/bash
# PocketGPU の Android 版を Gradle なしで組む。aapt2 → javac → d8 → zip → zipalign → apksigner
#   build_apk.sh [アプリの置き場]      既定は このフォルダの app（hello を渡すと Hello World）
# 出す物：out/<名前>.apk（名前は置き場のフォルダ名。app なら out/pocketgpu.apk）
# 使う物：D:\android-sdk の build-tools 35.0.0・platforms android-34、D:\tools\jdk-17。AndroidX などの外の部品は使わない
# GPU の受信アプリ：D:\iphone-rpc\androidbuild の server_aarch64・server_x86_64 を lib/<abi>/libpgpu_server.so として入れる
#   （app\vk\build_android.sh で組んだ物。PGPU_SERVER_DIR で置き場を変えられる。無ければ入れずに組む）
# 署名：keys/debug.keystore（無ければ作る。試験用の鍵。ストアに出すときは自分の鍵に変える）
set -e
SDK=${ANDROID_SDK:-/d/android-sdk}
BT=$SDK/build-tools/35.0.0
JAR=$SDK/platforms/android-34/android.jar
JDK=${JDK:-/d/tools/jdk-17}
HERE=$(cd "$(dirname "$0")" && pwd)
APP=$(cd "${1:-$HERE/app}" && pwd)
NAME=$(basename "$APP")
[ "$NAME" = app ] && NAME=pocketgpu
# OUT_NAME=pocketgpu_0.1.5 のように渡すと、out/pocketgpu.apk（キットが読む物）を書き換えずに別の名前で出せる
NAME=${OUT_NAME:-$NAME}
SRVDIR=${PGPU_SERVER_DIR:-/d/iphone-rpc/androidbuild}
MIN_SDK=29
TARGET_SDK=34
VCODE=${VCODE:-8}
VNAME=${VNAME:-0.1.7}
B=$HERE/out/build_$NAME
OUTAPK=$HERE/out/$NAME.apk
export JAVA_HOME=$JDK
JAVA=$JDK/bin/java
w() { cygpath -w "$1"; }

rm -rf "$B"
mkdir -p "$B/gen" "$B/classes" "$B/dex" "$B/stage" "$HERE/out" "$HERE/keys"

# 1. 画面の文や絵（res）を aapt2 で固めて、マニフェストと一緒に土台の apk にする。R.java もここで出る
RES_ZIP=""
if [ -d "$APP/res" ]; then
    "$BT/aapt2.exe" compile --dir "$(w "$APP/res")" -o "$(w "$B/res.zip")"
    RES_ZIP=$(w "$B/res.zip")
fi
"$BT/aapt2.exe" link -I "$(w "$JAR")" --manifest "$(w "$APP/AndroidManifest.xml")" -o "$(w "$B/base.apk")" \
    --java "$(w "$B/gen")" --min-sdk-version $MIN_SDK --target-sdk-version $TARGET_SDK \
    --version-code $VCODE --version-name $VNAME --auto-add-overlay $RES_ZIP

# 2. Java を組む（android.jar に対して。Java 8 の形にして d8 にラムダを直させる）
find "$APP/src" "$B/gen" -name '*.java' > "$B/sources.txt"
SRCLIST=$B/sources_w.txt
while read -r f; do w "$f"; done < "$B/sources.txt" > "$SRCLIST"
"$JDK/bin/javac" -J-Duser.language=en -J-Dstdout.encoding=UTF-8 -J-Dstderr.encoding=UTF-8 -encoding UTF-8 -source 8 -target 8 \
    -Xlint:-options -Xlint:deprecation -bootclasspath "$(w "$JAR");$(w "$BT/core-lambda-stubs.jar")" \
    -d "$(w "$B/classes")" @"$(w "$SRCLIST")"

# 3. d8 で classes.dex に
find "$B/classes" -name '*.class' > "$B/classes.txt"
CLS=$B/classes_w.txt
while read -r f; do w "$f"; done < "$B/classes.txt" > "$CLS"
"$JAVA" -cp "$(w "$BT/lib/d8.jar")" com.android.tools.r8.D8 --release --min-api $MIN_SDK --lib "$(w "$JAR")" \
    --output "$(w "$B/dex")" @"$(w "$CLS")"

# 4. 土台の apk に classes.dex と受信アプリ（lib/<abi>/libpgpu_server.so）を足す
cp "$B/dex/classes.dex" "$B/stage/classes.dex"
if [ -f "$APP/.with_gpu_server" ]; then
    for p in aarch64:arm64-v8a x86_64:x86_64; do
        src=$SRVDIR/server_${p%%:*}
        abi=${p##*:}
        if [ -f "$src" ]; then
            mkdir -p "$B/stage/lib/$abi"
            cp "$src" "$B/stage/lib/$abi/libpgpu_server.so"
            echo "受信アプリ：$src → lib/$abi/libpgpu_server.so"
        else
            echo "受信アプリが無い（$src）。GPU の機能なしで組む"
        fi
    done
fi
python -I - "$(w "$B/base.apk")" "$(w "$B/stage")" "$(w "$B/unsigned.apk")" <<'EOF'
import os, shutil, sys, zipfile
base, stage, out = sys.argv[1:4]
shutil.copyfile(base, out)
with zipfile.ZipFile(out, 'a', zipfile.ZIP_DEFLATED) as z:
    for root, _, files in os.walk(stage):
        for f in sorted(files):
            p = os.path.join(root, f)
            arc = os.path.relpath(p, stage).replace(os.sep, '/')
            # .so は縮めずに入れる（extractNativeLibs が true なので縮めてもよいが、入れる時間を短くする）
            z.write(p, arc, zipfile.ZIP_STORED if arc.endswith('.so') else zipfile.ZIP_DEFLATED)
EOF

# 5. 揃えて、署名する（.so は 4 KB 境目に揃える：-P 16 は 16 KB のページの機種向け）
"$BT/zipalign.exe" -P 16 -f 4 "$(w "$B/unsigned.apk")" "$(w "$B/aligned.apk")"
KS=$HERE/keys/debug.keystore
if [ ! -f "$KS" ]; then
    "$JDK/bin/keytool" -genkeypair -keystore "$(w "$KS")" -storepass android -keypass android -alias pocketgpu \
        -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=PocketGPU Debug" >/dev/null 2>&1
    echo "試験用の鍵を作った：$KS"
fi
"$JAVA" -jar "$(w "$BT/lib/apksigner.jar")" sign --ks "$(w "$KS")" --ks-pass pass:android --key-pass pass:android \
    --ks-key-alias pocketgpu --out "$(w "$OUTAPK")" "$(w "$B/aligned.apk")"
"$JAVA" -jar "$(w "$BT/lib/apksigner.jar")" verify "$(w "$OUTAPK")"
rm -f "$OUTAPK.idsig"
echo "できた：$OUTAPK（$(stat -c %s "$OUTAPK") バイト）"
