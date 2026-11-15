// Sampling profiler. Enabled by creating "profile.on" next to dmcr.exe.
// Every ~1 ms it suspends one busy thread of the process (round-robin over threads that used CPU in the last second),
// records EIP, and dumps per-thread histograms to profile_eip.txt every 10 s.
#include <tlhelp32.h>
struct PThread {
    DWORD tid; HANDLE h; ULONGLONG lastCpu; double cpuPct; DWORD total, outside; DWORD* hist; bool hot;
    struct { HMODULE m; char name[48]; DWORD n; } mods[24]; int nmods;
};
static PThread g_pt[64]; static int g_npt; static DWORD g_profSize;

static void ProfModule(PThread& t, DWORD eip) {
    HMODULE m = nullptr; GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)eip, &m);
    for (int i = 0; i < t.nmods; i++) if (t.mods[i].m == m) { t.mods[i].n++; return; }
    if (t.nmods < 24) { auto& e = t.mods[t.nmods++]; e.m = m; e.n = 1; char path[MAX_PATH] = "?"; if (m) GetModuleFileNameA(m, path, MAX_PATH); const char* b = strrchr(path, '\\'); strncpy(e.name, b ? b + 1 : path, 47); e.name[47] = 0; }
}
static void ProfRefreshThreads(DWORD selfTid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); THREADENTRY32 te = {sizeof te};
    if (snap == INVALID_HANDLE_VALUE) return;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == selfTid) continue;
        PThread* t = nullptr; for (int i = 0; i < g_npt; i++) if (g_pt[i].tid == te.th32ThreadID) t = &g_pt[i];
        if (!t) {
            if (g_npt >= 64) continue; t = &g_pt[g_npt++]; memset(t, 0, sizeof *t); t->tid = te.th32ThreadID;
            t->h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, t->tid); t->hist = (DWORD*)calloc(g_profSize, 4);
        }
        if (!t->h) continue; FILETIME c, e, k, u; if (GetThreadTimes(t->h, &c, &e, &k, &u)) {
            ULONGLONG cpu = (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime);
            t->cpuPct = (cpu - t->lastCpu) / 10000.0 / 10.0; /* % of 1 s window (100ns units) */ t->lastCpu = cpu; t->hot = t->cpuPct > 3.0;
        }
    }
    CloseHandle(snap);
}
static void ProfDump() {
    FILE* f = fopen("profile_eip.txt", "w"); if (!f) return; BYTE* base = (BYTE*)GetModuleHandleA(nullptr);
    fprintf(f, "# base=%p\n", base);
    for (int i = 0; i < g_npt; i++) { PThread& t = g_pt[i]; if (!t.total) continue;
        fprintf(f, "T %lu total=%lu outside=%lu lastcpu%%=%.0f mods:", t.tid, t.total, t.outside, t.cpuPct); for (int j = 0; j < t.nmods; j++) fprintf(f, " %s=%lu", t.mods[j].name, t.mods[j].n); fputc('\n', f); }
    for (int i = 0; i < g_npt; i++) { PThread& t = g_pt[i]; if (!t.total) continue;
        for (DWORD o = 0; o < g_profSize; o++) if (t.hist[o]) fprintf(f, "H %lu %08X %lu\n", t.tid, (unsigned)(DWORD)(base + o), t.hist[o]); }
    fclose(f);
}
static DWORD WINAPI ProfThread(LPVOID) {
    BYTE* base = (BYTE*)GetModuleHandleA(nullptr); auto nt = (PIMAGE_NT_HEADERS)(base + ((PIMAGE_DOS_HEADER)base)->e_lfanew); g_profSize = nt->OptionalHeader.SizeOfImage;
    DWORD self = GetCurrentThreadId(); while (!g_presentWnd) Sleep(20);
    Log("profiler: started (all busy threads)"); DWORD lastDump = GetTickCount(), lastRefresh = 0; int rr = 0;
    for (;;) {
        Sleep(1);
        if (GetTickCount() - lastRefresh > 1000) { lastRefresh = GetTickCount(); ProfRefreshThreads(self); }
        PThread* t = nullptr; for (int k = 0; k < g_npt && !t; k++) { rr = (rr + 1) % (g_npt ? g_npt : 1); if (g_pt[rr].hot && g_pt[rr].h && g_pt[rr].tid != GetCurrentThreadId()) t = &g_pt[rr]; }
        if (t && SuspendThread(t->h) != (DWORD)-1) {
            CONTEXT c; c.ContextFlags = CONTEXT_CONTROL; if (GetThreadContext(t->h, &c)) {
                DWORD off = c.Eip - (DWORD)base; t->total++;
                if (off < g_profSize) t->hist[off]++; else { t->outside++; ProfModule(*t, c.Eip); }
            }
            ResumeThread(t->h);
        }
        if (GetTickCount() - lastDump > 10000) { lastDump = GetTickCount(); ProfDump(); }
    }
}
static void ProfStart() {
    if (GetFileAttributesA("profile.on") == INVALID_FILE_ATTRIBUTES) return;
    CreateThread(nullptr, 0, ProfThread, nullptr, 0, nullptr);
}
