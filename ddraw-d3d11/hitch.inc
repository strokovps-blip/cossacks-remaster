// ---------------------------------------------------------------- hitch watchdog: WHY did a game tick take too long?
// The game thread marks the end of its work each time it enters the frame-limiter wait (My_PumpWait). If the main thread does not
// come back to the wait loop within 30 ms, this watchdog samples its stack (EBP chain: nearest exe return addresses + the module
// the EIP is in, i.e. exe / ntdll / dsound / winmm ...) every 4 ms until the tick ends, then logs one summary line:
//   "HITCH work 87.3 ms ... top: 004C5830(41%) 0047D1D0(22%) ... eip-in: dmcr.exe 70% ntdll.dll 25%"
// This attributes camera freezes to game code / sound / system without any action from the user.
static volatile DWORD g_mainTid;
static double HNow() { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return c.QuadPart * 1000.0 / f.QuadPart; }

static const char* ModName(DWORD addr, char* buf, size_t n) {
    HMODULE m = nullptr; buf[0] = 0;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &m) && m) { char p[MAX_PATH]; GetModuleFileNameA(m, p, MAX_PATH); const char* b = strrchr(p, '\\'); strncpy(buf, b ? b + 1 : p, n - 1); buf[n - 1] = 0; }
    else strcpy(buf, "?");
    return buf;
}
static DWORD WINAPI HitchThread(LPVOID) {
    while (!g_mainTid) Sleep(50);
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, g_mainTid); if (!th) return 0;
    BYTE* base = (BYTE*)GetModuleHandleA(nullptr);
    bool in = false; double hitchFrom = 0; int total = 0; std::map<DWORD, int> top1, top2, myRva; std::map<std::string, int> chains; std::map<std::string, int> mods; int logged = 0; HMODULE me = nullptr; GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&HitchThread, &me);
    for (;;) {
        Sleep(4); double now = HNow(), last = g_lastPumpExit;
        if (last <= 0) continue;
        if (now - last > 30) {                                         // the game has been working for 30+ ms
            if (!in) { in = true; hitchFrom = last; total = 0; top1.clear(); top2.clear(); myRva.clear(); chains.clear(); mods.clear(); }
            if (SuspendThread(th) != (DWORD)-1) {
                CONTEXT c; c.ContextFlags = CONTEXT_CONTROL; if (GetThreadContext(th, &c)) {
                    char mb[64]; mods[ModName(c.Eip, mb, sizeof mb)]++; total++; if (me && c.Eip >= (DWORD)me && c.Eip < (DWORD)me + 0x100000) myRva[(c.Eip - (DWORD)me) & ~0xF]++;
                    DWORD ebp = c.Ebp; int found = 0; std::string ch; char tb[16]; ch += (c.Eip >= 0x401000 && c.Eip < 0x700000) ? (sprintf(tb, "%06X ", c.Eip), tb) : "dll "; if (c.Eip >= 0x401000 && c.Eip < 0x700000) { top1[c.Eip & ~0xF]++; }
                    for (int d = 0; d < 40 && ebp > 0x10000 && !IsBadReadPtr((void*)ebp, 8); d++) { DWORD ret = ((DWORD*)ebp)[1], nx = ((DWORD*)ebp)[0]; if (ret >= 0x401000 && ret < 0x700000) { if (found == 0) top1[ret]++; found++; if (found <= 14) { sprintf(tb, "%06X ", ret); ch += tb; } if (found <= 3) top2[ret]++; } if (nx <= ebp) break; ebp = nx; }
                    if (found == 0) {                                  // no frame pointer chain (our own optimised code): scan the stack for return addresses
                        const DWORD* sp = (const DWORD*)c.Esp; int n = 0; ch += "| ";
                        for (int i = 0; i < 1024 && n < 12 && !IsBadReadPtr(sp + i, 4); i++) { DWORD v = sp[i]; if (v > 0x401005 && v < 0x700000 && !IsBadReadPtr((void*)(v - 5), 1) && *(BYTE*)(v - 5) == 0xE8) { sprintf(tb, "%06X ", v); ch += tb; n++; } }
                    }
                    chains[ch]++;
                }
                ResumeThread(th);
            }
        } else if (in) {                                               // the tick finished
            in = false; double dur = last - hitchFrom;
            if (logged++ < 60) {
                char line[900]; int o = sprintf(line, "HITCH game tick work ~%.0f ms (%d samples); cam [AF5E00]=%d [AF5E04]=%d; top code:", dur, total, GI(0xAF5E00), GI(0xAF5E04));
                std::vector<std::pair<int, DWORD>> v; for (auto& kv : top2) v.push_back({kv.second, kv.first}); std::sort(v.rbegin(), v.rend());
                for (size_t i = 0; i < v.size() && i < 6; i++) o += sprintf(line + o, " %08X(%d%%)", v[i].second, total ? v[i].first * 100 / total : 0);
                { std::vector<std::pair<int, DWORD>> rv; for (auto& kv : myRva) rv.push_back({kv.second, kv.first}); std::sort(rv.rbegin(), rv.rend()); o += sprintf(line + o, " | OUR DLL rva:"); for (size_t i = 0; i < rv.size() && i < 5; i++) o += sprintf(line + o, " %05X(%d%%)", rv[i].second, total ? rv[i].first * 100 / total : 0); }
                o += sprintf(line + o, " | EIP in:"); std::vector<std::pair<int, std::string>> mv; for (auto& kv : mods) mv.push_back({kv.second, kv.first}); std::sort(mv.rbegin(), mv.rend());
                for (size_t i = 0; i < mv.size() && i < 4; i++) o += sprintf(line + o, " %s %d%%", mv[i].second.c_str(), total ? mv[i].first * 100 / total : 0);
                Log("%s", line);
                { std::vector<std::pair<int, std::string>> cv; for (auto& kv : chains) cv.push_back({kv.second, kv.first}); std::sort(cv.rbegin(), cv.rend()); for (size_t i = 0; i < cv.size() && i < 3; i++) Log("  HITCH chain %d%%: %s", total ? cv[i].first * 100 / total : 0, cv[i].second.c_str()); }
            }
        }
    }
}
static void HitchStart() { if (GetFileAttributesA("hitch.off") == INVALID_FILE_ATTRIBUTES) CreateThread(nullptr, 0, HitchThread, nullptr, 0, nullptr); }
