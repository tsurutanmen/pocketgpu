// Linux で tests/*.c をそのまま組むための、<windows.h> の代わり（-Itests/linux_shim を付けると、試験の #include <windows.h> がこれになる）。
// 試験が使う分だけ：vulkan-1.dll を開く・関数を引く・時計・コンソールの文字コード
#pragma once
#ifndef _WIN32
#include <dlfcn.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

typedef void *HMODULE;
typedef struct { int64_t QuadPart; } LARGE_INTEGER;
#define CP_UTF8 65001

// "vulkan-1.dll" は Vulkan のローダー（libvulkan.so.1）に読み替える
static inline HMODULE LoadLibraryA(const char *name) {
    if (!strcmp(name, "vulkan-1.dll")) name = "libvulkan.so.1";
    return dlopen(name, RTLD_NOW);
}
static inline void *GetProcAddress(HMODULE m, const char *name) { return m ? dlsym(m, name) : NULL; }
static inline int QueryPerformanceFrequency(LARGE_INTEGER *f) { f->QuadPart = 1000000000; return 1; }
static inline int QueryPerformanceCounter(LARGE_INTEGER *c) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    c->QuadPart = (int64_t) ts.tv_sec * 1000000000 + ts.tv_nsec;
    return 1;
}
static inline int SetConsoleOutputCP(unsigned cp) { (void) cp; return 1; }
#endif
