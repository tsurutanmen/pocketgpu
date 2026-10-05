# iPhone では、Metal の「使ってよい目安」より、iOS が1つのアプリに許すメモリのほうが小さい。
# 受け手が答える空きメモリを、iOS が今許している量に合わせる（超えるとアプリが止められる）。
import sys
p = sys.argv[1] + '/ggml/src/ggml-metal/ggml-metal-device.m'
s = open(p, encoding='utf-8').read()
a = """        *free      = *total > cur ? *total - cur : 0;
    } else {"""
assert a in s, 'pattern not found'
s = s.replace(a, """        *free      = *total > cur ? *total - cur : 0;
#if TARGET_OS_IPHONE
        {
            const size_t avail  = os_proc_available_memory();
            const size_t margin = 512ull * 1024 * 1024; // 画面やほかの部品の分を残す
            const size_t ok     = avail > margin ? avail - margin : 0;
            if (*free > ok)        *free  = ok;
            if (*total > cur + ok) *total = cur + ok;
        }
#endif
    } else {""", 1)
s = s.replace('#include <Foundation/Foundation.h>', '#include <Foundation/Foundation.h>\n#include <TargetConditionals.h>\n#if TARGET_OS_IPHONE\n#include <os/proc.h>\n#endif', 1)
open(p, 'w', encoding='utf-8').write(s)
print('patched', p)
