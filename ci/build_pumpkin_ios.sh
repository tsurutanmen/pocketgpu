#!/bin/bash
# Pumpkin（Rust 製のマイクラのサーバー・GPL-3.0）を、iPhone のアプリに入れる静的ライブラリとして組む。
# GitHub Actions の pumpkin-ios.yml と同じ手順。できる物は pumpkin/target/aarch64-apple-ios/release/libpumpkin_ios.a
# プラグイン（WebAssembly）は iPhone で使えないので、mc/pumpkin-ios の入口はプラグイン無しで起動する
set -euo pipefail
cd "$(dirname "$0")/.."
command -v cargo > /dev/null || { curl -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal; . "$HOME/.cargo/env"; }
rustup target add aarch64-apple-ios

if [ ! -d pumpkin ]; then
    git clone https://github.com/Pumpkin-MC/Pumpkin.git pumpkin
    mkdir -p pumpkin/crates/pumpkin-ios
    cp mc/pumpkin-ios/Cargo.toml mc/pumpkin-ios/lib.rs pumpkin/crates/pumpkin-ios/
    # 入口を workspace に足す
    perl -0pi -e 's/members = \[/members = [\n    "crates\/pumpkin-ios",/' pumpkin/Cargo.toml
fi
git -C pumpkin log -1 --format="%H %cd"

cd pumpkin
cargo fetch --target aarch64-apple-ios -q
if ! grep -q '^\[patch.crates-io\]' Cargo.toml; then
    # nix・num_threads・sysinfo は iOS でも macOS の作りを使うが、Rust の libc は iOS 向けにそれを持っていない。
    # 写しを作ってその部分だけ直し、Pumpkin に写しの方を使わせる
    src=$(ls -d ~/.cargo/registry/src/*/nix-0.31.* | sort -V | tail -1)
    mkdir -p patched && rm -rf patched/nix && cp -R "$src" patched/nix
    python3 ../mc/patch_nix_ios.py patched/nix
    src2=$(ls -d ~/.cargo/registry/src/*/num_threads-0.1.* | sort -V | tail -1)
    rm -rf patched/num_threads && cp -R "$src2" patched/num_threads
    python3 ../mc/patch_num_threads_ios.py patched/num_threads
    src3=$(ls -d ~/.cargo/registry/src/*/sysinfo-0.38.* | sort -V | tail -1)
    rm -rf patched/sysinfo && cp -R "$src3" patched/sysinfo
    python3 ../mc/patch_sysinfo_ios.py patched/sysinfo
    printf '\n[patch.crates-io]\nnix = { path = "patched/nix" }\nnum_threads = { path = "patched/num_threads" }\nsysinfo = { path = "patched/sysinfo" }\n' >> Cargo.toml
fi
# アプリと同じ iOS 17 から動くように組む。直した nix は手元の部品として扱われ厳しい lint がかかるので、警告に落とす
IPHONEOS_DEPLOYMENT_TARGET=17.0 RUSTFLAGS="--cap-lints warn" cargo build -p pumpkin-ios --release --target aarch64-apple-ios 2>&1 | grep -E "^error|^\s+-->|Finished" | head -60
ls -la target/aarch64-apple-ios/release/libpumpkin_ios.a
