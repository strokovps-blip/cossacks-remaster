// ---------------------------------------------------------------- write-watch: which code writes the offscreen frame buffer?
// "trace.writes" next to dmcr.exe arms one capture: at the end of a frame the offscreen buffer (0x689088) is made read-only;
// during the next frame every first write to each 4 KB page faults, the vectored handler records the writer's EIP and its
// EBP-chain callers, unprotects the page and resumes. At the end of that frame trace_writes.txt lists, in time order, which
// function first painted each part of the screen (ground, objects, fog, interface ...).
struct WWRec { DWORD eip, c1, c2, c3, c4, addr, seq; };
static WWRec g_ww[2048]; static volatile LONG g_wwN, g_wwState;      // state: 0 idle, 1 armed (protect at next frame end), 2 recording
static BYTE* g_wwBase; static size_t g_wwLen; static DWORD g_wwFrame;

static LONG CALLBACK WWVeh(PEXCEPTION_POINTERS ep) {
    if (g_wwState != 2 || ep->ExceptionRecord->ExceptionCode != 0xC0000005 || ep->ExceptionRecord->ExceptionInformation[0] != 1) return EXCEPTION_CONTINUE_SEARCH;
    BYTE* a = (BYTE*)ep->ExceptionRecord->ExceptionInformation[1]; if (a < g_wwBase || a >= g_wwBase + g_wwLen) return EXCEPTION_CONTINUE_SEARCH;
    LONG i = InterlockedIncrement(&g_wwN) - 1;
    if (i < 2048) {
        WWRec& r = g_ww[i]; r.eip = ep->ContextRecord->Eip; r.addr = (DWORD)(a - g_wwBase); r.seq = i; r.c1 = r.c2 = r.c3 = r.c4 = 0;
        DWORD ebp = ep->ContextRecord->Ebp; DWORD* cs[4] = {&r.c1, &r.c2, &r.c3, &r.c4}; int k = 0;
        for (int d = 0; d < 16 && k < 4 && ebp > 0x10000 && !IsBadReadPtr((void*)ebp, 8); d++) { DWORD ret = ((DWORD*)ebp)[1], nx = ((DWORD*)ebp)[0]; if (ret >= 0x401000 && ret < 0x700000) *cs[k++] = ret; if (nx <= ebp) break; ebp = nx; }
    }
    DWORD old; VirtualProtect((void*)((DWORD)a & ~0xFFF), 4096, PAGE_READWRITE, &old);
    return EXCEPTION_CONTINUE_EXECUTION;
}
static void WWFrameEnd() {                                            // called by the CopyRect hook after every full-screen copy
    if (g_wwState == 0) { static DWORD lastChk; DWORD tk = GetTickCount(); if (tk - lastChk < 1000) return; lastChk = tk; if (GetFileAttributesA("trace.writes") != INVALID_FILE_ATTRIBUTES) { DeleteFileA("trace.writes"); g_wwState = 1; } else return; }
    BYTE* base = GP(0x689088); int pitch = GI(0x687FC8), h = GI(0x68889C); if (!base || pitch <= 0 || h <= 0) return;
    static bool installed; if (!installed) { AddVectoredExceptionHandler(1, WWVeh); installed = true; }
    if (g_wwState == 1) {                                             // protect now: the NEXT frame is recorded
        g_wwBase = (BYTE*)((DWORD)base & ~0xFFF); g_wwLen = (((DWORD)base + (size_t)pitch * h + 0xFFF) & ~0xFFF) - (DWORD)g_wwBase; g_wwN = 0;
        DWORD old; VirtualProtect(g_wwBase, g_wwLen, PAGE_READONLY, &old); g_wwState = 2; return;
    }
    if (g_wwState == 2) {                                             // frame finished: unprotect everything and dump
        g_wwState = 3; DWORD old; VirtualProtect(g_wwBase, g_wwLen, PAGE_READWRITE, &old);
        FILE* f = fopen("trace_writes.txt", "w"); if (f) {
            LONG n = g_wwN < 2048 ? g_wwN : 2048; fprintf(f, "# %ld page first-writes in one frame; buffer %p pitch %d height %d (4KB page = %d rows)\n# seq  page_row(y)  writer_eip  callers\n", n, base, pitch, h, 4096 / (pitch ? pitch : 1));
            for (LONG i = 0; i < n; i++) { WWRec& r = g_ww[i]; fprintf(f, "%4ld  y~%5d  %08X  %08X %08X %08X %08X\n", i, pitch ? (int)(r.addr / pitch) : 0, r.eip, r.c1, r.c2, r.c3, r.c4); }
            fclose(f); }
        Log("write-watch dumped (%ld faults)", g_wwN); g_wwState = 0;
    }
}
