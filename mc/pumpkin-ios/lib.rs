// iPhone のアプリの中で Pumpkin（Rust 製のマイクラのサーバー）を動かす入口。
// Pumpkin 本体の main.rs から、プラグイン（WebAssembly。iPhone では使えない JIT が要る）とシグナルの扱いを抜いたもの。
// アプリは別のスレッドで pumpkin_ios_run(作業の場所) を呼ぶ。サーバーが止まるまで戻らない。
use std::ffi::CStr;
use std::os::raw::c_char;
use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::Ordering;

use pumpkin_config::{LoadConfiguration, PumpkinConfig};
use pumpkin_core::data::VanillaData;
use pumpkin_core::plugin::loader::PluginLoader;
use pumpkin_core::{PumpkinServer, SERVER_EXIT_CODE};
use tracing::info;

/// # Safety
/// dir は NUL で終わる UTF-8 の文字列であること
#[unsafe(no_mangle)]
pub unsafe extern "C" fn pumpkin_ios_run(dir: *const c_char) -> i32 {
    let dir = if dir.is_null() {
        PathBuf::from(".")
    } else {
        PathBuf::from(unsafe { CStr::from_ptr(dir) }.to_string_lossy().into_owned())
    };
    let _ = std::env::set_current_dir(&dir);
    let rt = match tokio::runtime::Builder::new_multi_thread().enable_all().thread_name("pumpkin").build() {
        Ok(rt) => rt,
        Err(e) => {
            eprintln!("Pumpkin：tokio を始められない：{e}");
            return 1;
        }
    };
    rt.block_on(run(dir))
}

async fn run(dir: PathBuf) -> i32 {
    let _ = rustls::crypto::ring::default_provider().install_default();
    let _ = rayon::ThreadPoolBuilder::new()
        .thread_name(|i| format!("Rayon-Worker-{i}"))
        .build_global();
    let config = PumpkinConfig::load(&dir);
    let vanilla_data = VanillaData::load();
    pumpkin_core::init_logger(&config.advanced);
    info!("Pumpkin を iPhone の中で始める（作業の場所 {}）", dir.display());
    let plugin_loaders: Vec<Arc<dyn PluginLoader>> = Vec::new();
    let server = match PumpkinServer::new(config.basic, config.advanced, config.telemetry, vanilla_data, plugin_loaders).await {
        Ok(s) => s,
        Err(e) => {
            tracing::error!("世界を用意できない：{e}");
            return 1;
        }
    };
    let _ = server.init_plugins().await;
    info!("サーバーが動いた");
    server.start().await;
    info!("サーバーが止まった");
    SERVER_EXIT_CODE.load(Ordering::Acquire)
}
