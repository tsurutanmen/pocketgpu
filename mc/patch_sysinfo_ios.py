"""sysinfo 0.38 を iOS 向けに組めるようにする。
sysinfo は iOS でも macOS と同じ調べ方（proc_pidinfo・mach_host_self など）を使うが、Rust の libc は iOS 向けにそれを持っていない。
iOS では「対応していない OS」向けの版（unknown。何も調べず空を返す）を使わせる。Pumpkin が使うのは状態の表示だけ。
  python3 patch_sysinfo_ios.py sysinfo の写しの場所
"""
import sys

p = sys.argv[1].rstrip('/') + '/src/lib.rs'
s = open(p, encoding='utf-8').read()
old = '''        target_os = "macos", target_os = "ios",
        target_os = "linux", target_os = "android",'''
assert s.count(old) == 1, 'sysinfo の書き方が変わった'
s = s.replace(old, '''        target_os = "macos",
        target_os = "linux", target_os = "android",''')
open(p, 'w', encoding='utf-8').write(s)
print('patched sysinfo')
