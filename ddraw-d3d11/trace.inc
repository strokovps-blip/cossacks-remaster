// ---------------------------------------------------------------- draw-call tracing (understanding how the game builds a frame)
// Creating "trace.now" next to dmcr.exe captures the next 3 game frames (a frame = one full-screen CopyRect) of every
// GP-sprite draw issued through the generic dispatcher 0047C1EF:
//     thiscall DrawSprite(GPfile*, x, y, spriteIndex, frame|flags, color)
// and writes trace_frames.txt (all draws in order) plus a summary by caller. The file is deleted once the capture started.
struct TraceRec { DWORD frame, ret, self, x, y, id, fr, col; };
#define TRACE_MAX 150000
static TraceRec* g_trace; static volatile LONG g_traceN, g_traceActive;
static volatile LONG g_frameSeq, g_traceFrom = -1, g_traceTo = -1, g_traceDumped;
static volatile LONG g_putImgCalls, g_copyCalls;

extern "C" void __cdecl TraceDrawSprite(DWORD* retp, DWORD self) {
    LONG f = g_frameSeq;
    if (f < g_traceFrom || f >= g_traceTo || !g_trace) return;
    LONG i = InterlockedIncrement(&g_traceN) - 1; if (i >= TRACE_MAX) return;
    TraceRec& r = g_trace[i]; r.frame = f; r.ret = retp[0]; r.self = self; r.x = retp[1]; r.y = retp[2]; r.id = retp[3]; r.fr = retp[4]; r.col = retp[5];
}
static void __declspec(naked) DrawSpriteThunk() {
    __asm {
        pushad
        pushfd
        lea eax, [esp + 36]
        push ecx
        push eax
        call TraceDrawSprite
        add esp, 8
        popfd
        popad
        push ebp                      // displaced prologue of 0047C1EF (6 bytes)
        mov ebp, esp
        sub esp, 0x44
        push 0x47C1F5
        ret
    }
}
static volatile LONG g_callsN;
static void CallsDump(); static LONG LayerHits();
static void TraceDump() {
    FILE* f = fopen("trace_frames.txt", "w"); if (!f) return;
    LONG n = min((LONG)g_traceN, (LONG)TRACE_MAX);
    fprintf(f, "# %ld sprite draws captured over frames %ld..%ld\n# fields: frame ret self x y spriteIndex frame|flags color\n", n, g_traceFrom, g_traceTo - 1);
    for (LONG i = 0; i < n; i++) { TraceRec& r = g_trace[i]; fprintf(f, "%lu %08X %08X %d %d %lu %lX %lu\n", r.frame, r.ret, r.self, (int)r.x, (int)r.y, r.id, r.fr, r.col); }
    fclose(f);
    FILE* s = fopen("trace_summary.txt", "w"); if (!s) return;
    std::map<DWORD, int> byRet, bySelf; std::map<DWORD, int> perFrame;
    for (LONG i = 0; i < n; i++) { byRet[g_trace[i].ret]++; bySelf[g_trace[i].self]++; perFrame[g_trace[i].frame]++; }
    fprintf(s, "draws per frame:"); for (auto& kv : perFrame) fprintf(s, " %lu:%d", kv.first, kv.second); fprintf(s, "\nCallers (return address -> draws):\n");
    for (auto& kv : byRet) fprintf(s, "  %08X %d\n", kv.first, kv.second);
    fprintf(s, "GP file objects (this -> draws):\n"); for (auto& kv : bySelf) fprintf(s, "  %08X %d\n", kv.first, kv.second);
    fclose(s);
    Log("trace dumped: %ld draws", n);
}
static void TracePoll() {                                      // called from the presenter loop
    if (!g_trace) return;
    static DWORD lastChk; DWORD tk = GetTickCount(); if (g_traceFrom < 0 && tk - lastChk < 1000) return; lastChk = tk;
#ifdef REMASTER_DEV
    static DWORD tFirst, nCap; if (LayerHits() > 0 && !tFirst) tFirst = tk;       // developer build: capture 3 frames 25 s and 50 s after the mission started
    if (g_traceFrom < 0 && tFirst && nCap < 2 && tk - tFirst > 25000u * (nCap + 1)) { nCap++; HANDLE h = CreateFileA("trace.now", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr); if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
#endif
    if (g_traceFrom < 0 && GetFileAttributesA("trace.now") != INVALID_FILE_ATTRIBUTES) {
        DeleteFileA("trace.now"); g_traceN = 0; g_callsN = 0; g_traceFrom = g_frameSeq + 2; g_traceTo = g_traceFrom + 3; g_traceDumped = 0; Log("trace armed: frames %ld..%ld", g_traceFrom, g_traceTo - 1);
    }
    if (g_traceFrom >= 0 && g_frameSeq > g_traceTo && !g_traceDumped) { g_traceDumped = 1; TraceDump(); CallsDump(); g_traceFrom = g_traceTo = -1; }
}
