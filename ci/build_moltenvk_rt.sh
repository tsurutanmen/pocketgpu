#!/bin/bash
# レイトレーシングの入った作りかけの MoltenVK（KhronosGroup/MoltenVK の PR #2771）を iOS 用に組み、
# 公開版の MoltenVK-ios.tar と同じ形（MoltenVK/MoltenVK/include と static）の tar.xz にする。Codemagic の Mac で動かす。
#   ci/build_moltenvk_rt.sh [出す所]      既定は ./mvk-rt
# 出来る物：出す所/MoltenVK-ios-rt.tar.xz と MOLTENVK_RT_COMMIT.txt。記録は出す所/*.log（失敗しても残る）
# アプリの組み立て（ci/build_ios.sh）は MVK_RT=true のとき、vendor/moltenvk-rt/ の tar.xz か、ここで組んだ物を使う。
# 注意
#  - PR は作りかけで、作者は M5 の Mac でしか試していない。本家の CI の「Xcode - ios」は 176926b で通っている（2026-10-06）。
#  - 拡張を名乗るのは MVK_CONFIG_ENABLE_EXPERIMENTAL_RAY_TRACING=1 のときだけ（アプリの設定「レイトレーシングを試す」で立てる）。
#  - descriptor set の作りも変わっている（レビューで競合の心配を言われた）。レイトレーシングを切っていても、ふだん使いの IPA には入れない。
#  - SPIRV-Cross は PR の fetchDependencies が作者の fork から取る（ExternalRevisions/SPIRV-Cross_repo_revision）。
set -euo pipefail
OUT=${1:-$PWD/mvk-rt}
# PR #2771 の先頭（2026-10-06「Give each any-hit candidate its own invocation in the traversal loop」）。40 文字で書く
MVK_COMMIT=${MVK_COMMIT:-176926b4eb7b6bd70f46a59f277b4417dd235f67}
mkdir -p "$OUT/src"
OUT=$(cd "$OUT" && pwd)
SRC="$OUT/src"
T0=$(date +%s)
step() { echo "== $1（$(( $(date +%s) - T0 )) 秒）"; }
keep_logs() {
    cp -f "$SRC/dependenciesbuild.log" "$OUT/fetchDependencies.log" 2>/dev/null || true
    cp -f "$SRC/xcodebuild.log" "$OUT/xcodebuild.log" 2>/dev/null || true
}
fail() {
    keep_logs
    echo "失敗：$1"
    for f in "$OUT/fetchDependencies.log" "$OUT/xcodebuild.log"; do
        [ -f "$f" ] && { echo "---- $f の終わり"; grep -n -E "error:|fatal|BUILD FAILED" "$f" | tail -40 || true; tail -40 "$f"; }
    done
    exit 1
}

step "MoltenVK を取る $MVK_COMMIT"
cd "$SRC"
[ -d .git ] || git init -q
# PR の commit は本家の refs/pull/2771/head からも届く。本家で取れなければ作者の fork（dttdrv/MoltenVK の macgaming/ray-query-pr）から
if ! git fetch -q --depth 1 https://github.com/KhronosGroup/MoltenVK.git "$MVK_COMMIT"; then
    git fetch -q --depth 1 https://github.com/dttdrv/MoltenVK.git "$MVK_COMMIT" || fail "commit $MVK_COMMIT が取れない"
fi
git checkout -q --force FETCH_HEAD
git log -1 --format='MoltenVK %H %cd %s'
echo "SPIRV-Cross：$(cat ExternalRevisions/SPIRV-Cross_repo_revision)"

step "依存（SPIRV-Cross・SPIRV-Tools・Vulkan-Headers など）を iOS 用に組む"
# 依存の組んだ物は Codemagic の cache（External/build）に置く。取る版が同じなら組み直さない（本家の CI と同じやり方）
WANT=$(cat fetchDependencies ExternalRevisions/* | shasum | cut -c1-40)
STAMP=External/build/RT_DEPS_STAMP
if [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$WANT" ] && ls -d External/build/Release/*.xcframework > /dev/null 2>&1; then
    echo "依存は cache の物を使う"
    ./fetchDependencies --none || fail "fetchDependencies --none"
else
    ./fetchDependencies --ios --parallel-build || fail "fetchDependencies --ios"
    echo "$WANT" > "$STAMP"
fi

step "MoltenVK を iOS 用に組む"
make ios || fail "make ios"
LIB=Package/Release/MoltenVK/static/MoltenVK.xcframework/ios-arm64/libMoltenVK.a
[ -f "$LIB" ] || fail "$LIB が無い"
ls -la "$LIB"

step "レイトレーシングの名前が入ったかを確かめる"
for n in VK_KHR_acceleration_structure VK_KHR_ray_query VK_KHR_deferred_host_operations; do
    if grep -a -q -F "$n" "$LIB"; then echo "あり：$n"; else fail "$n が入っていない"; fi
done
# 次の 2 つは無くても止めない（名前の作り方が違うだけかもしれない）
for n in VK_KHR_ray_tracing_pipeline ENABLE_EXPERIMENTAL_RAY_TRACING; do
    if grep -a -q -F "$n" "$LIB"; then echo "あり：$n"; else echo "見つからない（続ける）：$n"; fi
done

step "公開版の tar と同じ形にして縮める"
rm -rf "$OUT/pkg" && mkdir -p "$OUT/pkg/MoltenVK/MoltenVK"
cp -R Package/Release/MoltenVK/include "$OUT/pkg/MoltenVK/MoltenVK/"
cp -R Package/Release/MoltenVK/static "$OUT/pkg/MoltenVK/MoltenVK/"
touch "$OUT/pkg/MoltenVK/MoltenVK/RT_BUILD" # 公開版と見分ける印（ci/build_ios.sh が見る）
( cd "$OUT/pkg" && tar cf - MoltenVK ) | xz -T0 -6 > "$OUT/MoltenVK-ios-rt.tar.xz"
git log -1 --format='%H %cd PR #2771' > "$OUT/MOLTENVK_RT_COMMIT.txt"
keep_logs
ls -la "$OUT/MoltenVK-ios-rt.tar.xz"
step "できた"
