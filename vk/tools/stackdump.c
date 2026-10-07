// 固まったプロセスの、全部の糸が今どの関数にいるかを書き出す。デバッガが無い PC 用。
//   stackdump.exe <プロセス番号>
// 関数名は dll の横の .pdb から引く（ドライバと受け手は -g で作っている）
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>

static void frame_name(HANDLE ph, DWORD64 pc, char *out, size_t n) {
    IMAGEHLP_MODULE64 mi;
    memset(&mi, 0, sizeof mi);
    mi.SizeOfStruct = sizeof mi;
    const char *mod = SymGetModuleInfo64(ph, pc, &mi) ? mi.ModuleName : "?";
    char sbuf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *si = (SYMBOL_INFO *) sbuf;
    memset(sbuf, 0, sizeof sbuf);
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 255;
    DWORD64 disp = 0;
    if (SymFromAddr(ph, pc, &disp, si))
        snprintf(out, n, "%s!%s+0x%llx", mod, si->Name, (unsigned long long) disp);
    else
        snprintf(out, n, "%s+0x%llx", mod, (unsigned long long) (pc - mi.BaseOfImage));
}

int main(int argc, char **argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 2) { fprintf(stderr, "使い方: stackdump <プロセス番号>\n"); return 2; }
    DWORD pid = (DWORD) strtoul(argv[1], NULL, 10);
    HANDLE ph = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!ph) { fprintf(stderr, "開けない（%lu）\n", GetLastError()); return 1; }
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    if (!SymInitialize(ph, NULL, TRUE)) { fprintf(stderr, "シンボルを読めない（%lu）\n", GetLastError()); return 1; }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    te.dwSize = sizeof te;
    int nth = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!th) continue;
        SuspendThread(th);
        CONTEXT c;
        memset(&c, 0, sizeof c);
        c.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(th, &c)) {
            printf("== 糸 %lu\n", te.th32ThreadID);
            STACKFRAME64 sf;
            memset(&sf, 0, sizeof sf);
            sf.AddrPC.Offset = c.Rip; sf.AddrPC.Mode = AddrModeFlat;
            sf.AddrFrame.Offset = c.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
            sf.AddrStack.Offset = c.Rsp; sf.AddrStack.Mode = AddrModeFlat;
            for (int i = 0; i < 40; i++) {
                if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, ph, th, &sf, &c, NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL)) break;
                if (!sf.AddrPC.Offset) break;
                char nm[400];
                frame_name(ph, sf.AddrPC.Offset, nm, sizeof nm);
                IMAGEHLP_LINE64 ln;
                memset(&ln, 0, sizeof ln);
                ln.SizeOfStruct = sizeof ln;
                DWORD d2 = 0;
                if (SymGetLineFromAddr64(ph, sf.AddrPC.Offset, &d2, &ln)) {
                    const char *f = strrchr(ln.FileName, '\\');
                    printf("  %s  %s:%lu\n", nm, f ? f + 1 : ln.FileName, ln.LineNumber);
                } else
                    printf("  %s\n", nm);
            }
            nth++;
        }
        ResumeThread(th);
        CloseHandle(th);
    }
    CloseHandle(snap);
    SymCleanup(ph);
    CloseHandle(ph);
    printf("糸 %d 本\n", nth);
    return 0;
}
