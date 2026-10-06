"""nix 0.31 を iOS 向けに組めるようにする。
nix は「Apple の OS」全部で、カーネル拡張の制御用ソケット（sockaddr_ctl・SYSPROTO_CONTROL・utun）を使えるとしているが、
Rust の libc は iOS 向けにそれを持っていない。その部分だけを macOS に絞る。
  python3 patch_nix_ios.py nix の写しの場所
"""
import re
import sys

root = sys.argv[1].rstrip('/') + '/src/sys/socket/'


def patch(name, pairs):
    p = root + name
    s = open(p, encoding='utf-8').read()
    for pat, rep in pairs:
        s2, n = re.subn(pat, rep, s)
        assert n > 0, (name, pat)
        s = s2
    open(p, 'w', encoding='utf-8').write(s)
    print('patched', name)


MAC = 'target_os = "macos"'
patch('addr.rs', [
    (r'#\[cfg\(all\(feature = "ioctl", apple_targets\)\)\]', f'#[cfg(all(feature = "ioctl", {MAC}))]'),
    (r'#\[cfg\(apple_targets\)\](\s*#\[cfg\(feature = "ioctl"\)\]\s*\(?libc::AF_SYSTEM)', f'#[cfg({MAC})]\\1'),
    (r'#\[cfg\(apple_targets\)\](\s*pub mod sys_control)', f'#[cfg({MAC})]\\1'),
])
patch('mod.rs', [
    (r'#\[cfg\(apple_targets\)\](\s*#\[cfg\(feature = "ioctl"\)\]\s*pub use crate::sys::socket::addr::sys_control::SysControlAddr;)', f'#[cfg({MAC})]\\1'),
    (r'#\[cfg\(apple_targets\)\](\s*KextControl)', f'#[cfg({MAC})]\\1'),
])
patch('sockopt.rs', [
    (r'#\[cfg\(apple_targets\)\](\s*#\[cfg\(feature = "net"\)\]\s*sockopt_impl!\(\s*/// Get the utun interface name\.)', f'#[cfg({MAC})]\\1'),
])
