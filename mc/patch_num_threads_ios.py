"""num_threads 0.1 を iOS 向けに組めるようにする。
iOS でも macOS と同じ数え方（mach_task_self など）を使うが、Rust の libc は iOS 向けにそれを持っていない。
iOS では「数えられない（None）」と答える版（imp.rs）を使わせる。数えられないとき、time などは安全な側に倒れる。
  python3 patch_num_threads_ios.py num_threads の写しの場所
"""
import sys

p = sys.argv[1].rstrip('/') + '/src/lib.rs'
s = open(p, encoding='utf-8').read()
old = '#[cfg_attr(any(target_os = "macos", target_os = "ios"), path = "apple.rs")]'
assert s.count(old) == 1, 'num_threads の書き方が変わった'
s = s.replace(old, '#[cfg_attr(target_os = "macos", path = "apple.rs")]')
open(p, 'w', encoding='utf-8').write(s)
print('patched num_threads')
