// ---------------------------------------------------------------- variant B, stage 1: the world as a list of leaf-blitter calls (developer build)
// The 22 leaf blitters (14 RLE variants 00477E0F..0047BCAD with 5 cdecl args, 8 table variants 004711AE..004726A7 with 3-4 args) all start with
// `push ebp; mov ebp,esp; sub esp,imm8` and take (x, y, ...) as their first two arguments; they write into the offscreen buffer [689088] (pitch [689068],
// clip rectangle [689080..689078] x [689084..68907C], inclusive).
//   1. Entry hooks record every call of the world phase (from the ground blit 445796 until the fog passes 4E2E3D): (blitter, args, clip).
//   2. A sprite "atlas" is built by BLACK-BOX probing: the game's own blitter draws the sprite into a scratch canvas over several uniform backgrounds;
//      each pixel becomes: skip (background shows through) / opaque index / table T (out = T[background], shadows and transparency).
//   3. The CPU reference compositor replays ground + records + atlas and is compared with the game's real pre-fog frame (log: mismatch count per blitter).
// When the reference matches, the same composition is a mechanical port to a GPU pass (next stage).
#ifdef REMASTER_GPUW
#include <array>
#include <memory>
static bool BytesEq(DWORD addr, const char* hex); static bool WriteJmp(DWORD from, void* to, int totalLen);
struct LeafDef { DWORD addr; int nargs; };
#define NLEAF 26
static const LeafDef kLeaf[NLEAF] = {{0x477E0F, 5}, {0x4782B5, 5}, {0x47874C, 5}, {0x478C10, 5}, {0x4790D4, 5}, {0x4795E3, 5}, {0x479AA7, 5}, {0x479F53, 5}, {0x47A411, 5}, {0x47A8E7, 5}, {0x47ADC9, 5}, {0x47B293, 5}, {0x47B769, 5}, {0x47BCAD, 5},
                                  {0x4711AE, 3}, {0x471495, 3}, {0x471774, 4}, {0x471A6E, 4}, {0x471D7A, 4}, {0x472083, 4}, {0x472398, 4}, {0x4726A7, 4}, {0x442AAE, 3},
                                  {0x401000, 4}, {0x401100, 4}, {0x4012FF, 5}};      // line primitives: horizontal (x1,y,x2,c), vertical (x,y1,y2,c), general (x1,y1,x2,y2,c) - selection marks, bars
// ---- data handed to the presenter thread (GPU compositor): everything one world frame needs
#define ATLAS_W 4096
#define ATLAS_H 8192
#define GW_MAXREC 8192
#define GW_MAXIDX (1 << 20)
struct GwRec { int bx, by, w, h, ax, ay, cx0, cx1, cy0, cy1; };
struct GpuWorldJob { LONG seq = 0; int W = 0, H = 0, P = 0, tilesW = 0, tilesH = 0; std::vector<BYTE> base; std::vector<GwRec> recs; std::vector<unsigned> tileOff, tileIdx;
                     bool fogOn = false; int cols = 0, rows = 0, ox = 0, oy = 0; std::vector<unsigned> cells; std::vector<BYTE> lut; bool ok = false; };
static double g_tRecMs, g_tCsWaitMs, g_tSprMs, g_tTileMs; static long g_nRec;
static CRITICAL_SECTION g_gwCs; static std::vector<unsigned short> g_atlasBits; static int g_shelfX, g_shelfY, g_shelfH, g_atlasDirtyY0 = 1 << 30, g_atlasDirtyY1 = -1; static bool g_atlasFull;
static std::shared_ptr<GpuWorldJob> g_jobPub, g_jobBuild; static volatile LONG g_gwFault, g_gwEpoch;     // fault: the GPU world is switched off for this run (a hook faulted / the check failed)
struct LeafRec { unsigned char id; int a[5]; int clip[4]; };                  // clip: minX, maxX, minY, maxY
static bool g_gndPending; static std::vector<LeafRec> g_rec; static volatile LONG g_recOn, g_inProbe; static BYTE* g_recBuf; static std::vector<BYTE> g_gnd;        // ground snapshot (after the ground blit)
static int g_recW, g_recH, g_recPitch;

struct SprEntry2; static SprEntry2* GetSprite(int id, const int* a);
static volatile LONG g_attrOn; static void AttrStep(int tag); static long g_skipDest[NLEAF];
extern "C" void __cdecl RecordLeaf(int id, DWORD* retp) {
    if (!g_recOn || g_inProbe || g_gwFault) return;
    if (g_attrOn) { AttrStep(100 + id); }
    if (GP(0x689088) != g_recBuf) { g_skipDest[id]++; return; }                // drawing into another buffer (ground cache refill, our probes)
    if (g_gndPending) { g_gndPending = false; g_gnd.assign(g_recBuf, g_recBuf + (size_t)g_recPitch * g_recH); }       // base = the buffer when the first sprite call arrives (terrain, water and everything else drawn before it)
    LeafRec r; r.id = (unsigned char)id; for (int i = 0; i < 5; i++) r.a[i] = i < kLeaf[id].nargs ? (int)retp[1 + i] : 0;
    r.clip[0] = GI(0x689080); r.clip[1] = GI(0x689078); r.clip[2] = GI(0x689084); r.clip[3] = GI(0x68907C);
    if (kLeaf[id].addr == 0x401000) r.a[2] -= r.a[0];                         // lines: store the extent relative to the start
    else if (kLeaf[id].addr == 0x401100) r.a[2] -= r.a[1];
    else if (kLeaf[id].addr == 0x4012FF) { r.a[2] -= r.a[0]; r.a[3] -= r.a[1]; }
    if (kLeaf[id].addr == 0x442AAE) {                                          // its data is a temporary (often on the stack): key by content and probe NOW, while the data is valid
        unsigned h = 2166136261u; __try { const BYTE* p = (const BYTE*)(DWORD)r.a[2]; unsigned rows = *(const unsigned short*)p; h = (h ^ *(const unsigned*)(p + 4)) * 16777619u; h = (h ^ *(const unsigned*)(p + 8)) * 16777619u; h = (h ^ rows) * 16777619u;
            const BYTE* q = p + 12; if (rows > 4096) rows = 4096; for (unsigned i = 0; i < rows; i++) { unsigned len = *(const unsigned short*)(q + 2); for (unsigned k = 0; k < 4 + len; k++) h = (h ^ q[k]) * 16777619u; q += 4 + len; } } __except (EXCEPTION_EXECUTE_HANDLER) { h = 0; }
        r.a[3] = (int)h; GetSprite(id, r.a); }
    if (g_rec.size() < 200000) g_rec.push_back(r);
}
static void* g_leafStub[NLEAF];
static void InstallLeafHooks(int& ok) {
    BYTE* mem = (BYTE*)VirtualAlloc(nullptr, NLEAF * 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE); int n = 0;
    for (int id = 0; id < NLEAF; id++) {
        if (!BytesEq(kLeaf[id].addr, "558BEC83EC")) { Log("leaf %X: prologue mismatch, skipped", kLeaf[id].addr); continue; }
        BYTE* q = mem + id * 64; BYTE* p = q; g_leafStub[id] = q;
        *p++ = 0x60; *p++ = 0x9C;                                                // pushad; pushfd
        *p++ = 0x8D; *p++ = 0x44; *p++ = 0x24; *p++ = 0x24;                      // lea eax,[esp+36]  (return address; arguments follow)
        *p++ = 0x50;                                                             // push eax
        *p++ = 0x68; *(int*)p = id; p += 4;                                      // push id
        *p++ = 0xB9; *(void**)p = (void*)&RecordLeaf; p += 4;                    // mov ecx, RecordLeaf
        *p++ = 0xFF; *p++ = 0xD1;                                                // call ecx
        *p++ = 0x83; *p++ = 0xC4; *p++ = 0x08;                                   // add esp,8
        *p++ = 0x9D; *p++ = 0x61;                                                // popfd; popad
        memcpy(p, (void*)kLeaf[id].addr, 6); p += 6;                             // displaced prologue (push ebp; mov ebp,esp; sub esp,imm8)
        *p++ = 0xE9; *(int*)p = (int)(kLeaf[id].addr + 6 - (DWORD)(p + 4)); p += 4;
        if (WriteJmp(kLeaf[id].addr, q, 6)) n++;
    }
    InitializeCriticalSection(&g_gwCs); g_atlasBits.assign((size_t)ATLAS_W * ATLAS_H, 0);
    ok += n; Log("sprite leaf hooks installed: %d of %d", n, NLEAF);
}

// ---------------------------------------------------------------- attribution profiler (one frame): which function changed how many pixels of the frame buffer
#include "probe_table.inc"
static std::vector<BYTE> g_attrSnap; static int g_attrPrev = -1; static long g_attrCalls;
struct AttrStat { long px = 0, calls = 0; int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1; };
static std::map<int, AttrStat> g_attr;                                         // tag: 0..45 probes, 100+id leaf blitters, 999 frame end
static void AttrStep(int tag) {
    if (!g_attrOn) return; BYTE* buf = GP(0x689088); int P = g_recPitch, W = g_recW, H = g_recH; if (buf != g_recBuf) return;
    if (++g_attrCalls > 30000) { g_attrOn = 0; return; }
    AttrStat& cur = g_attr[g_attrPrev]; 
    for (int y = 0; y < H; y++) { BYTE* a = buf + (size_t)y * P; BYTE* b = g_attrSnap.data() + (size_t)y * P; if (memcmp(a, b, W) == 0) continue;
        for (int x = 0; x < W; x++) if (a[x] != b[x]) { cur.px++; if (x < cur.x0) cur.x0 = x; if (x > cur.x1) cur.x1 = x; if (y < cur.y0) cur.y0 = y; if (y > cur.y1) cur.y1 = y; }
        memcpy(b, a, W); }
    g_attr[tag].calls++; g_attrPrev = tag;
}
extern "C" void __cdecl ProbeEnter(int id) { AttrStep(id); }
static void InstallProbeHooks(int& ok) {
    const int n = sizeof(kProbe) / sizeof(kProbe[0]); BYTE* mem = (BYTE*)VirtualAlloc(nullptr, n * 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE); int cnt = 0;
    for (int i = 0; i < n; i++) {
        char hex[40]; for (int k = 0; k < kProbe[i].len; k++) sprintf(hex + 2 * k, "%02X", kProbe[i].bytes[k]);
        if (!BytesEq(kProbe[i].addr, hex)) { Log("probe %X: prologue mismatch", kProbe[i].addr); continue; }
        BYTE* q = mem + i * 64; BYTE* p = q;
        *p++ = 0x60; *p++ = 0x9C; *p++ = 0x68; *(int*)p = i; p += 4; *p++ = 0xB9; *(void**)p = (void*)&ProbeEnter; p += 4; *p++ = 0xFF; *p++ = 0xD1; *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04; *p++ = 0x9D; *p++ = 0x61;
        memcpy(p, kProbe[i].bytes, kProbe[i].len); p += kProbe[i].len; *p++ = 0xE9; *(int*)p = (int)(kProbe[i].addr + kProbe[i].len - (DWORD)(p + 4)); p += 4;
        if (WriteJmp(kProbe[i].addr, q, kProbe[i].len)) cnt++;
    }
    ok += cnt; Log("attribution probes installed: %d of %d", cnt, n);
}
// ---------------------------------------------------------------- write tracer (one frame): which instruction addresses write the frame buffer, with the written area
static volatile LONG g_wtOn; static BYTE* g_wtBase; static size_t g_wtLen; static DWORD g_wtStepPage; static void* g_wtVeh; static long g_wtFaults;
struct WtStat { long n = 0; int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1; };
static std::map<DWORD, WtStat> g_wt; static std::map<DWORD, std::string> g_wtNote;
static LONG CALLBACK WtVeh(PEXCEPTION_POINTERS ep) {
    DWORD c = ep->ExceptionRecord->ExceptionCode;
    if (c == EXCEPTION_ACCESS_VIOLATION && g_wtOn && ep->ExceptionRecord->NumberParameters >= 2 && ep->ExceptionRecord->ExceptionInformation[0] == 1) {
        BYTE* a = (BYTE*)ep->ExceptionRecord->ExceptionInformation[1]; if (a < g_wtBase || a >= g_wtBase + g_wtLen) return EXCEPTION_CONTINUE_SEARCH;
        size_t off = a - g_wtBase; int y = (int)(off / g_recPitch), x = (int)(off % g_recPitch); DWORD eip = (DWORD)ep->ContextRecord->Eip; bool known = g_wt.count(eip) != 0; WtStat& st = g_wt[eip]; st.n++; g_wtFaults++;
        if (!known && !(eip >= 0x401000 && eip < 0x5C0000)) {                                 // writer outside the exe image (generated code?): keep registers and the exe addresses found on its stack
            char l[500]; int n = _snprintf(l, sizeof l - 1, "eax %08X ebx %08X ecx %08X edx %08X esi %08X edi %08X ebp %08X esp %08X stack:", ep->ContextRecord->Eax, ep->ContextRecord->Ebx, ep->ContextRecord->Ecx, ep->ContextRecord->Edx, ep->ContextRecord->Esi, ep->ContextRecord->Edi, ep->ContextRecord->Ebp, ep->ContextRecord->Esp);
            __try { const DWORD* sp = (const DWORD*)ep->ContextRecord->Esp; int f = 0; for (int i = 0; i < 64 && f < 10; i++) if (sp[i] >= 0x401000 && sp[i] < 0x5C0000) { n += _snprintf(l + n, sizeof l - n - 1, " %X@%d", sp[i], i); f++; } } __except (EXCEPTION_EXECUTE_HANDLER) {}
            l[n] = 0; g_wtNote[eip] = l; }
        if (x < st.x0) st.x0 = x; if (x > st.x1) st.x1 = x; if (y < st.y0) st.y0 = y; if (y > st.y1) st.y1 = y;
        DWORD old; BYTE* pg = (BYTE*)((DWORD)a & ~4095u); VirtualProtect(pg, 4096, PAGE_READWRITE, &old); g_wtStepPage = (DWORD)pg; ep->ContextRecord->EFlags |= 0x100; return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (c == EXCEPTION_SINGLE_STEP && g_wtStepPage) { DWORD old; VirtualProtect((void*)g_wtStepPage, 4096, PAGE_READONLY, &old); g_wtStepPage = 0; return EXCEPTION_CONTINUE_EXECUTION; }
    return EXCEPTION_CONTINUE_SEARCH;
}
static void WtStart(BYTE* buf, size_t len) {
    if (!g_wtVeh) g_wtVeh = AddVectoredExceptionHandler(1, WtVeh);
    g_wt.clear(); g_wtNote.clear(); g_wtFaults = 0; g_wtStepPage = 0; g_wtBase = (BYTE*)((DWORD)buf & ~4095u); g_wtLen = ((len + (buf - g_wtBase)) + 4095) & ~4095u; DWORD old; g_wtOn = 1; VirtualProtect(g_wtBase, g_wtLen, PAGE_READONLY, &old);
}
static void WtStop() {
    if (!g_wtOn) return; DWORD old; VirtualProtect(g_wtBase, g_wtLen, PAGE_READWRITE, &old); g_wtOn = 0;
    std::vector<std::pair<long, DWORD>> v; for (auto& kv : g_wt) v.push_back({kv.second.n, kv.first}); std::sort(v.rbegin(), v.rend());
    Log("writetrace: %ld write faults, %zu distinct writer addresses", g_wtFaults, v.size());
    for (size_t i = 0; i < v.size() && i < 40; i++) { WtStat& s2 = g_wt[v[i].second]; Log("writetrace: eip %08X writes %ld bbox (%d,%d)-(%d,%d)", v[i].second, s2.n, s2.x0, s2.y0, s2.x1, s2.y1); }
    for (auto& kv : g_wtNote) Log("writetrace: outside-image writer %08X: %s", kv.first, kv.second.c_str());
}
// ---------------------------------------------------------------- atlas by black-box probing
struct SprEntry { int ox = 0, oy = 0, w = 0, h = 0; bool ok = false; std::vector<unsigned char> kind, val; };      // kind: 0 skip, 1 opaque (val = index), 2 table (val = table id mod 256) ; tid: see tbl
struct SprEntry2 : SprEntry { std::vector<unsigned short> tid; int ax = -1, ay = -1; unsigned chk = 0; bool par = false; };
static std::map<std::array<int, 5>, SprEntry2> g_atlas; static std::vector<std::array<unsigned char, 256>> g_tbl; static std::map<std::string, int> g_tblId;
static std::vector<BYTE> g_canvas; static long g_probeCount, g_probeFail; static double g_probeMs;

static void CallLeaf(int id, int x, int y, const int* a) {
    typedef void(__cdecl * F5)(int, int, int, int, int); typedef void(__cdecl * F4)(int, int, int, int); DWORD f = kLeaf[id].addr;
    if (f == 0x401000) { ((F4)f)(x, y, x + a[2], a[3]); return; }
    if (f == 0x401100) { ((F4)f)(x, y, y + a[2], a[3]); return; }
    if (f == 0x4012FF) { ((F5)f)(x, y, x + a[2], y + a[3], a[4]); return; }
    ((F5)f)(x, y, a[2], a[3], a[4]);
}
// draw the sprite with the game's blitter into canvas (cw x ch, pitch cw) at (x, y); canvas memory is ours, clip = canvas
static void ProbeDraw(int id, const int* a, int x, int y, int cw, int ch, BYTE bg) {
    memset(g_canvas.data(), bg, (size_t)cw * ch);
    int s0 = GI(0x689088), s1 = GI(0x689068), s2 = GI(0x689078), s3 = GI(0x68907C), s4 = GI(0x689080), s5 = GI(0x689084);
    GI(0x689088) = (int)g_canvas.data(); GI(0x689068) = cw; GI(0x689080) = 0; GI(0x689078) = cw - 1; GI(0x689084) = 0; GI(0x68907C) = ch - 1;
    InterlockedExchange(&g_inProbe, 1);
    __try { CallLeaf(id, x, y, a); } __except (EXCEPTION_EXECUTE_HANDLER) { g_probeFail++; }
    InterlockedExchange(&g_inProbe, 0);
    GI(0x689088) = s0; GI(0x689068) = s1; GI(0x689078) = s2; GI(0x68907C) = s3; GI(0x689080) = s4; GI(0x689084) = s5;
}
#include <set>
struct PendReq { int id; int a[5]; };                                           // a sprite the frame needed that is not probed yet: done later, in the game's idle time
static std::vector<PendReq> g_pend; static std::set<std::array<int, 5>> g_pendSet;
static void Enqueue(int id, const int* a) {
    if (g_pend.size() > 30000) return; int par = (a[0] & 1) | ((a[1] & 1) << 1); std::array<int, 5> k = {id, a[4], a[2], a[3], par}; if (!g_pendSet.insert(k).second) return;
    PendReq r; r.id = id; for (int i = 0; i < 5; i++) r.a[i] = a[i]; g_pend.push_back(r);
}
static bool g_probeAllowed = true, g_probeDeferred;                              // job build: probing is limited per frame; a sprite that has to wait makes this frame use the game's own layer
static void PackEntry(SprEntry2& e) {                                                                          // put a finished entry into the GPU atlas (shelf packer): 0 skip, 1<<14|index opaque, 2<<14|table
    if (e.w <= 0 || g_atlasBits.empty() || g_atlasFull) return;
    { double w0 = QpcMs(); EnterCriticalSection(&g_gwCs); g_tCsWaitMs += QpcMs() - w0; }
    if (g_shelfX + e.w > ATLAS_W) { g_shelfY += g_shelfH; g_shelfX = 0; g_shelfH = 0; }
    if (e.w > ATLAS_W || g_shelfY + e.h > ATLAS_H) g_atlasFull = true;
    else { e.ax = g_shelfX; e.ay = g_shelfY; g_shelfX += e.w; if (e.h > g_shelfH) g_shelfH = e.h;
        for (int y = 0; y < e.h; y++) for (int x = 0; x < e.w; x++) { size_t j = (size_t)y * e.w + x; unsigned short v = e.kind[j] == 0 ? 0 : e.kind[j] == 1 ? (unsigned short)(0x4000 | e.val[j]) : (unsigned short)(0x8000 | e.tid[j] | (e.par ? 0x2000 : 0)); g_atlasBits[(size_t)(e.ay + y) * ATLAS_W + e.ax + x] = v; }
        if (e.ay < g_atlasDirtyY0) g_atlasDirtyY0 = e.ay; if (e.ay + e.h - 1 > g_atlasDirtyY1) g_atlasDirtyY1 = e.ay + e.h - 1; }
    LeaveCriticalSection(&g_gwCs);
}
static bool ProbeEntry(int id, const int* a, int par, bool pack, SprEntry2& e) {        // par: parity of the call position (x&1 | (y&1)<<1) - dithered shadows depend on it
    double t0 = QpcMs(); if (par) e.par = true;                                  // parity variants belong to a dithered sprite
    static const BYTE bgs[9] = {0, 32, 64, 96, 128, 160, 192, 224, 255};
    for (int attempt = 0; attempt < 3 && !e.ok; attempt++) {
        static const int sz[3] = {256, 768, 1536}; int cw = sz[attempt], ch = cw, ox = cw / 2 + (par & 1), oy = ch / 2 + (par >> 1); g_canvas.assign((size_t)cw * ch + 4096, 0);
        std::vector<BYTE> out[9];
        for (int b = 0; b < 9; b++) { ProbeDraw(id, a, ox, oy, cw, ch, bgs[b]); out[b].assign(g_canvas.begin(), g_canvas.begin() + (size_t)cw * ch); }
        int x0 = cw, y0 = ch, x1 = -1, y1 = -1; std::vector<unsigned char> st((size_t)cw * ch, 0);      // st: 0 identity, 1 opaque, 2 dependent
        for (int y = 0; y < ch; y++) for (int x = 0; x < cw; x++) {
            size_t i = (size_t)y * cw + x; bool opaque = true, ident = true;
            for (int b = 0; b < 9; b++) { if (out[b][i] != out[0][i]) opaque = false; if (out[b][i] != bgs[b]) ident = false; }
            st[i] = ident ? 0 : opaque ? 1 : 2; if (st[i]) { if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
        }
        if (x1 < 0) { if (attempt < 2) continue; e.ok = true; e.w = e.h = 0; break; }                      // draws nothing (the sprite offset may put it outside a small canvas: retry bigger)
        if (x0 <= 0 || y0 <= 0 || x1 >= cw - 1 || y1 >= ch - 1) { if (attempt < 2) continue; g_probeFail++; break; }   // touches the border: retry bigger / give up
        e.ox = x0 - ox; e.oy = y0 - oy; e.w = x1 - x0 + 1; e.h = y1 - y0 + 1; e.kind.assign((size_t)e.w * e.h, 0); e.val.assign((size_t)e.w * e.h, 0); e.tid.assign((size_t)e.w * e.h, 0);
        int dx0 = cw, dy0 = ch, dx1 = -1, dy1 = -1;
        for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
            size_t i = (size_t)y * cw + x, j = (size_t)(y - y0) * e.w + (x - x0);
            if (st[i] == 1) { e.kind[j] = 1; e.val[j] = out[0][i]; }
            else if (st[i] == 2) { if (x < dx0) dx0 = x; if (x > dx1) dx1 = x; if (y < dy0) dy0 = y; if (y > dy1) dy1 = y; }
        }
        if (dx1 >= 0) {                                                                                      // dependent pixels: full 256-background probe over their bounding box only
            int bw = dx1 - dx0 + 1, bh = dy1 - dy0 + 1, padX = 2 + (((ox - dx0 + 2) ^ ox) & 1), padY = 2 + (((oy - dy0 + 2) ^ oy) & 1), cw2 = bw + 2 * padX, ch2 = bh + 2 * padY;     // padding keeps the parity of the draw position
            std::vector<std::vector<BYTE>> V(256); g_canvas.assign((size_t)cw2 * ch2 + 4096, 0);
            for (int b = 0; b < 256; b++) { ProbeDraw(id, a, ox - dx0 + padX, oy - dy0 + padY, cw2, ch2, (BYTE)b); V[b].assign(g_canvas.begin(), g_canvas.begin() + (size_t)cw2 * ch2); }
            for (int y = dy0; y <= dy1; y++) for (int x = dx0; x <= dx1; x++) {
                size_t i = (size_t)y * cw + x; if (st[i] != 2) continue; size_t k = (size_t)(y - dy0 + padY) * cw2 + (x - dx0 + padX), j = (size_t)(y - y0) * e.w + (x - x0);
                bool ident = true, cst = true; for (int b = 0; b < 256; b++) { if (V[b][k] != b) ident = false; if (V[b][k] != V[0][k]) cst = false; }
                if (ident) continue; if (cst) { e.kind[j] = 1; e.val[j] = V[0][k]; continue; }
                std::string s(256, 0); for (int b = 0; b < 256; b++) s[b] = (char)V[b][k];
                auto f = g_tblId.find(s); int id2; if (f == g_tblId.end()) { id2 = (int)g_tbl.size(); std::array<unsigned char, 256> T; memcpy(T.data(), s.data(), 256); { double w0 = QpcMs(); EnterCriticalSection(&g_gwCs); g_tCsWaitMs += QpcMs() - w0; } g_tbl.push_back(T); LeaveCriticalSection(&g_gwCs); g_tblId[s] = id2; } else id2 = f->second;
                e.kind[j] = 2; e.tid[j] = (unsigned short)id2;
            }
        }
        e.ok = true;
        if (pack && e.w > 0) PackEntry(e);
    }
    g_probeMs += QpcMs() - t0; return e.ok;
}
// Does the picture depend on the parity of the draw position? (dithered shadows draw every other pixel starting at the screen column/row parity)
// Cheap test on a small canvas around the sprite: draw at (x,y), (x+1,y), (x,y+1) over two backgrounds and compare in sprite-relative coordinates.
static bool ParityDependent(int id, const int* a, const SprEntry2& e) {
    if (!e.ok || e.w <= 0) return false;
    int cw = e.w + 12, ch = e.h + 12, tx = 4 - e.ox, ty = 4 - e.oy; tx += tx & 1; ty += ty & 1; if (tx + e.ox + e.w + 2 > cw || ty + e.oy + e.h + 2 > ch) { cw += 2; ch += 2; }     // even origin = parity 0
    if (cw * ch > 1000000) return false;
    static const BYTE bg2[2] = {0, 255}; std::vector<BYTE> ref[2];
    for (int b = 0; b < 2; b++) { g_canvas.assign((size_t)cw * ch + 4096, 0); ProbeDraw(id, a, tx, ty, cw, ch, bg2[b]); ref[b].assign(g_canvas.begin(), g_canvas.begin() + (size_t)cw * ch); }
    for (int par = 1; par <= 2; par++) { int px = par & 1, py = par >> 1;
        for (int b = 0; b < 2; b++) { g_canvas.assign((size_t)cw * ch + 4096, 0); ProbeDraw(id, a, tx + px, ty + py, cw, ch, bg2[b]);
            for (int y = 0; y + py < ch; y++) for (int x = 0; x + px < cw; x++) if (g_canvas[(size_t)(y + py) * cw + x + px] != ref[b][(size_t)y * cw + x]) {
                if (x >= 1 && y >= 1 && x < cw - 2 && y < ch - 2) return true; } } }
    return false;
}
static bool SameEntry(const SprEntry2& x, const SprEntry2& y) { return x.ok == y.ok && x.ox == y.ox && x.oy == y.oy && x.w == y.w && x.h == y.h && x.kind == y.kind && x.val == y.val && x.tid == y.tid; }
static void MarkDither(SprEntry2& e) {                                                                       // the base entry was packed before its dither property was known: flag its table pixels now
    if (e.ax < 0 || e.w <= 0) return; EnterCriticalSection(&g_gwCs);
    for (int y = 0; y < e.h; y++) for (int x = 0; x < e.w; x++) if (e.kind[(size_t)y * e.w + x] == 2) g_atlasBits[(size_t)(e.ay + y) * ATLAS_W + e.ax + x] |= 0x2000;
    if (e.ay < g_atlasDirtyY0) g_atlasDirtyY0 = e.ay; if (e.ay + e.h - 1 > g_atlasDirtyY1) g_atlasDirtyY1 = e.ay + e.h - 1; LeaveCriticalSection(&g_gwCs);
}
static SprEntry2* GetSprite(int id, const int* a) {
    std::array<int, 5> key = {id, a[4], 0, a[2], a[3]}; int par = (a[0] & 1) | ((a[1] & 1) << 1);
    auto it = g_atlas.find(key);
    if (it == g_atlas.end()) {
        if (!g_probeAllowed) { g_probeDeferred = true; Enqueue(id, a); return nullptr; }
        SprEntry2& e = g_atlas[key]; g_probeCount++; ProbeEntry(id, a, 0, true, e);
        if (e.ok && e.w > 0) { double t1 = QpcMs(); e.par = ParityDependent(id, a, e); g_probeMs += QpcMs() - t1; if (e.par) MarkDither(e); }
        return e.ok ? &e : nullptr;
    }
    SprEntry2& e = it->second; if (!e.ok) return nullptr; if (!e.par || par == 0) return &e;
    key[2] = par; auto v = g_atlas.find(key); if (v != g_atlas.end()) return v->second.ok ? &v->second : nullptr;
    if (!g_probeAllowed) { g_probeDeferred = true; Enqueue(id, a); return nullptr; }
    SprEntry2& ve = g_atlas[key]; g_probeCount++; ProbeEntry(id, a, par, true, ve); return ve.ok ? &ve : nullptr;
}

// ---------------------------------------------------------------- reference compositor + comparison with the real pre-fog frame
struct SpriteStats { long frames = 0, recs = 0, noSprite = 0, px = 0, diff = 0, diffFrames = 0; long perLeaf[NLEAF] = {}; long perLeafRecs[NLEAF] = {}; double ms = 0; long firstX = -1, firstY = -1, firstLeaf = -1; };
static std::vector<BYTE> g_preFogCmp, g_preFogReal; static SpriteStats g_ss; static std::vector<BYTE> g_cmp; static std::vector<signed char> g_lastLeaf;

static void SpritesFrameBegin() {                                              // called right after the ground blit
    int pitch = GI(0x687FC8), w = GI(0x6888A0), h = GI(0x68889C); BYTE* src = GP(0x689088); if (!src || pitch <= 0 || w <= 0 || h <= 0) return;
    g_recBuf = src; g_recW = w; g_recH = h; g_recPitch = pitch; g_gnd.clear(); g_gndPending = true; g_rec.clear(); InterlockedExchange(&g_recOn, 1);
    if (g_ss.frames == 0x7fffffff) WtStart(src, (size_t)pitch * h);
    if (g_ss.frames == 0x7ffffffe) { g_attr.clear(); g_attrCalls = 0; g_attrPrev = 0; g_attrSnap.assign(src, src + (size_t)pitch * h); g_attr[0]; g_attrOn = 1; }
}
static void BuildGpuJob(int W, int H, int P);
static void SafeBuildJob(int W, int H, int P) { BuildGpuJob(W, H, P); }
static void BuildGpuJob(int W, int H, int P) {
    // GPU job: base + resolved records + per-tile lists (order preserved)
    {
        double tJob0 = QpcMs();
        std::shared_ptr<GpuWorldJob> job = std::make_shared<GpuWorldJob>(); job->W = W; job->H = H; job->P = P; 
#ifdef REMASTER_DEV
        job->base = g_gnd;
#else
        job->base.swap(g_gnd);
#endif
        if (g_atlasFull) {                                                                                      // the atlas is full: start over with what is on screen now (a long scroll collects far more sprites than one view needs)
            EnterCriticalSection(&g_gwCs); g_atlas.clear(); g_shelfX = g_shelfY = g_shelfH = 0; g_atlasFull = false; g_jobPub.reset(); LeaveCriticalSection(&g_gwCs); Log("gpuworld: sprite atlas was full, emptied and rebuilt from the current view"); }
        job->tilesW = (W + 15) / 16; job->tilesH = (H + 15) / 16; bool bad = g_atlasFull;
        g_probeDeferred = false; g_probeAllowed = true;
        g_probeAllowed = false; for (const LeafRec& r : g_rec) { SprEntry2* e = GetSprite(r.id, r.a); if (!e || !e->w) continue; if (e->ax < 0) { bad = true; break; }
            GwRec q; q.bx = r.a[0] + e->ox; q.by = r.a[1] + e->oy; q.w = e->w; q.h = e->h; q.ax = e->ax; q.ay = e->ay; q.cx0 = r.clip[0]; q.cx1 = r.clip[1]; q.cy0 = r.clip[2]; q.cy1 = r.clip[3]; job->recs.push_back(q); }
        g_probeAllowed = true; if (g_probeDeferred) bad = true;                                // sprites still being probed: this frame is shown from the game's own layer
        g_tSprMs += QpcMs() - tJob0;
        if ((int)job->recs.size() > GW_MAXREC) bad = true;
        if (!bad) { size_t nt = (size_t)job->tilesW * job->tilesH; std::vector<unsigned> cnt(nt + 1, 0); auto span = [&](const GwRec& q, int& tx0, int& tx1, int& ty0, int& ty1) { int x0 = (std::max)((std::max)(q.bx, q.cx0), 0), x1 = (std::min)((std::min)(q.bx + q.w - 1, q.cx1), W - 1), y0 = (std::max)((std::max)(q.by, q.cy0), 0), y1 = (std::min)((std::min)(q.by + q.h - 1, q.cy1), H - 1); if (x1 < x0 || y1 < y0) { tx0 = 1; tx1 = 0; ty0 = 1; ty1 = 0; return; } tx0 = x0 >> 4; tx1 = x1 >> 4; ty0 = y0 >> 4; ty1 = y1 >> 4; };
            for (const GwRec& q : job->recs) { int a, b, c, d; span(q, a, b, c, d); for (int ty = c; ty <= d; ty++) for (int tx = a; tx <= b; tx++) cnt[(size_t)ty * job->tilesW + tx + 1]++; }
            for (size_t i = 0; i < nt; i++) cnt[i + 1] += cnt[i]; job->tileOff = cnt; job->tileIdx.assign(cnt[nt], 0); if (cnt[nt] > GW_MAXIDX) bad = true;
            else { std::vector<unsigned> pos(cnt.begin(), cnt.end() - 1); unsigned ri = 0; for (const GwRec& q : job->recs) { int a, b, c, d; span(q, a, b, c, d); for (int ty = c; ty <= d; ty++) for (int tx = a; tx <= b; tx++) job->tileIdx[pos[(size_t)ty * job->tilesW + tx]++] = ri; ri++; } } }
        g_tTileMs += QpcMs() - tJob0;
        job->ok = !bad; g_jobBuild = job;
        { static long nb, nok, nFull, nDef, nAx, nRec, nIdx; static double lastL; nb++; if (!bad) nok++; if (g_atlasFull) nFull++; if (g_probeDeferred) nDef++; if ((int)job->recs.size() > GW_MAXREC) nRec++;
          double nowL = QpcMs(); if (nowL - lastL > 5000) { lastL = nowL; Log("gpuworld job: %ld built, %ld ok | refused: atlas full %ld, probing %ld, too many records %ld; %zu records, atlas %zu sprites, %zu tables", nb, nok, nFull, nDef, nRec, job->recs.size(), g_atlas.size(), g_tbl.size()); } }
    }
}
static void SpritesIdleWork(double budgetMs) {                                  // called from the game's frame-limiter wait: the game thread has nothing to do there, so sprites are probed without any visible stall
    if (g_pend.empty() || g_gwFault) return; double t0 = QpcMs();
    while (!g_pend.empty() && QpcMs() - t0 < budgetMs) {
        PendReq r = g_pend.back(); g_pend.pop_back(); g_pendSet.erase(std::array<int, 5>{r.id, r.a[4], r.a[2], r.a[3], (r.a[0] & 1) | ((r.a[1] & 1) << 1)});
        GetSprite(r.id, r.a); GetSprite(r.id, r.a); }                                // second call creates the parity variant when the base sprite turned out to be dithered
}
extern "C" void __cdecl SpritesPreFog() {                                      // called right before the fog passes: the world without fog is in the buffer
    if (!g_recOn) return; InterlockedExchange(&g_recOn, 0);
    WtStop();
    BYTE* src = GP(0x689088); if (src != g_recBuf) return; if (g_gndPending) { g_gndPending = false; g_gnd.assign(src, src + (size_t)g_recPitch * g_recH); } if ((int)g_gnd.size() != g_recPitch * g_recH) return;
#ifndef REMASTER_DEV
    SafeBuildJob(g_recW, g_recH, g_recPitch); return;                                // GPU-world build: no CPU reference composition, only the job for the presenter
#endif
    if (g_attrOn) { AttrStep(999); g_attrOn = 0;
        std::vector<std::pair<long, int>> v; for (auto& kv : g_attr) v.push_back({kv.second.px, kv.first}); std::sort(v.rbegin(), v.rend());
        for (auto& pr : v) { if (pr.first == 0) break; AttrStat& a = g_attr[pr.second]; DWORD ad = pr.second >= 100 && pr.second < 200 ? kLeaf[pr.second - 100].addr : pr.second < (int)(sizeof(kProbe) / sizeof(kProbe[0])) ? kProbe[pr.second].addr : 0;
            Log("attr: %08X (%s) changed %ld px in %ld calls, bbox (%d,%d)-(%d,%d)", ad, pr.second >= 100 ? "leaf" : "probe", a.px, a.calls, a.x0, a.y0, a.x1, a.y1); }
        Log("attr: %ld hook entries observed%s", g_attrCalls, g_attrCalls > 30000 ? " (limit hit, partial)" : ""); }
#ifdef REMASTER_DEV
#endif
    double t0 = QpcMs(); int W = g_recW, H = g_recH, P = g_recPitch; g_cmp = g_gnd; g_lastLeaf.assign((size_t)P * H, -1);
    long noSpr = 0;
    for (const LeafRec& r : g_rec) {
        g_ss.perLeafRecs[r.id]++;
        SprEntry2* e = GetSprite(r.id, r.a); if (!e) { noSpr++; continue; } if (!e->w) continue;
        int bx = r.a[0] + e->ox, by = r.a[1] + e->oy;
        for (int y = 0; y < e->h; y++) { int py = by + y; if (py < r.clip[2] || py > r.clip[3] || py < 0 || py >= H) continue;
            for (int x = 0; x < e->w; x++) { int px = bx + x; if (px < r.clip[0] || px > r.clip[1] || px < 0 || px >= W) continue;
                size_t j = (size_t)y * e->w + x; unsigned char k = e->kind[j]; if (!k) continue;
                size_t i = (size_t)py * P + px; g_cmp[i] = k == 1 ? e->val[j] : g_tbl[e->tid[j]][g_cmp[i]]; g_lastLeaf[i] = (signed char)r.id; } }
    }
    if (g_ss.frames == 400 || g_ss.frames == 150) { char l2[400]; int n2 = 0; for (int i = 0; i < NLEAF; i++) if (g_skipDest[i]) n2 += _snprintf(l2 + n2, sizeof l2 - n2 - 1, " %X:%ld", kLeaf[i].addr, g_skipDest[i]); l2[n2] = 0; Log("  leaf calls into other buffers while recording (cumulative):%s", l2); }
    if (g_ss.frames == 400) {                                                          // dev: the 25 biggest sprites of the frame (buildings?) and what the atlas made of them
        std::vector<std::pair<long, size_t>> big; for (size_t k = 0; k < g_rec.size(); k++) { SprEntry2* e = GetSprite(g_rec[k].id, g_rec[k].a); if (e && e->w) big.push_back({(long)e->w * e->h, k}); }
        std::sort(big.rbegin(), big.rend()); for (size_t q = 0; q < big.size() && q < 25; q++) { const LeafRec& r = g_rec[big[q].second]; SprEntry2* e = GetSprite(r.id, r.a); int c1 = 0, c2 = 0;
            for (size_t jj = 0; jj < e->kind.size(); jj++) { c1 += e->kind[jj] == 1; c2 += e->kind[jj] == 2; }
            Log("  big rec #%zu leaf %X args %d %d %08X %08X %08X clip %d %d %d %d -> sprite %dx%d at %d,%d opaque %d table %d", big[q].second, kLeaf[r.id].addr, r.a[0], r.a[1], r.a[2], r.a[3], r.a[4], r.clip[0], r.clip[1], r.clip[2], r.clip[3], e->w, e->h, r.a[0] + e->ox, r.a[1] + e->oy, c1, c2); } }
    long diff = 0, px = 0; long perLeaf[NLEAF] = {}; int fx = -1, fy = -1, fl = -1;
    for (int y = 0; y < H; y++) { const BYTE* a = src + (size_t)y * P; const BYTE* b = g_cmp.data() + (size_t)y * P;
        if (memcmp(a, b, W) == 0) { px += W; continue; }
        for (int x = 0; x < W; x++) { px++; if (a[x] != b[x]) { diff++; int l = g_lastLeaf[(size_t)y * P + x]; if (l >= 0) perLeaf[l]++; if (fx < 0) { fx = x; fy = y; fl = l; } } } }
    g_ss.frames++; g_ss.recs += (long)g_rec.size(); g_ss.noSprite += noSpr; g_ss.px += px; g_ss.diff += diff; if (diff) g_ss.diffFrames++; g_ss.ms += QpcMs() - t0;
    for (int i = 0; i < NLEAF; i++) g_ss.perLeaf[i] += perLeaf[i]; if (diff && g_ss.firstX < 0) { g_ss.firstX = fx; g_ss.firstY = fy; g_ss.firstLeaf = fl; }
    { static double lastEvt; static int evts; double nowE = QpcMs();                                                // dev: large mismatches (e.g. while scrolling): which blitter, and pictures
        if (diff > 150 && nowE - lastEvt > 2500 && evts < 8) { lastEvt = nowE; evts++; char line[600]; int n = 0; for (int i = 0; i < NLEAF; i++) if (perLeaf[i]) n += _snprintf(line + n, sizeof line - n - 1, " %X:%ld", kLeaf[i].addr, perLeaf[i]); line[n] = 0;
            Log("diffevent %d: %ld px differ, by last blitter:%s ; first at (%d,%d) blitter %d; camera %d,%d", evts, diff, line, fx, fy, fl, GI(0xAF5E00), GI(0xAF5E04));
            Pal* dp = g_lastPal; char fn[64]; int bright = 0, bl = -1; for (int i = 0; i < 256 && dp; i++) { int l = dp->e[i].peRed * 30 + dp->e[i].peGreen * 59 + dp->e[i].peBlue * 11; if (l > bl) { bl = l; bright = i; } }
            std::vector<BYTE> dm((size_t)P * H); for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { size_t i = (size_t)y * P + x; dm[i] = src[i] != g_cmp[i] ? (BYTE)bright : (BYTE)(g_cmp[i] == (BYTE)bright ? 0 : g_cmp[i]); }
            sprintf(fn, "evt%d_real.png", evts); DumpIndexed(fn, src, P, W, H, 0, 0, W, H, dp, 1); sprintf(fn, "evt%d_cmp.png", evts); DumpIndexed(fn, g_cmp.data(), P, W, H, 0, 0, W, H, dp, 1); sprintf(fn, "evt%d_diff.png", evts); DumpIndexed(fn, dm.data(), P, W, H, 0, 0, W, H, dp, 1); } }
    if (g_ss.frames == 150 || g_ss.frames == 400) {                                  // evidence images: the real pre-fog frame, our composition, and where they differ
        Pal* dp = g_lastPal; char fn[64]; int bright = 0, bl = -1; for (int i = 0; i < 256 && dp; i++) { int l = dp->e[i].peRed * 30 + dp->e[i].peGreen * 59 + dp->e[i].peBlue * 11; if (l > bl) { bl = l; bright = i; } }
        std::vector<BYTE> dm((size_t)P * H); for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { size_t i = (size_t)y * P + x; dm[i] = src[i] != g_cmp[i] ? (BYTE)bright : (BYTE)(g_cmp[i] == (BYTE)bright ? 0 : g_cmp[i]); }
        sprintf(fn, "spr%ld_real.png", g_ss.frames); DumpIndexed(fn, src, P, W, H, 0, 0, W, H, dp, 1);
        sprintf(fn, "spr%ld_cmp.png", g_ss.frames); DumpIndexed(fn, g_cmp.data(), P, W, H, 0, 0, W, H, dp, 1);
        sprintf(fn, "spr%ld_diff.png", g_ss.frames); DumpIndexed(fn, dm.data(), P, W, H, 0, 0, W, H, dp, 1);
        sprintf(fn, "spr%ld_ground.png", g_ss.frames); DumpIndexed(fn, g_gnd.data(), P, W, H, 0, 0, W, H, dp, 1); Log("sprites: evidence images spr%ld_*.png written", g_ss.frames);
    }
    SafeBuildJob(W, H, P);
    g_preFogCmp = g_cmp; g_preFogReal.assign(src, src + (size_t)P * H);
    if (diff && g_ss.frames <= 2 && fx >= 0) {                                       // dev: which records cover the first differing pixel, and what do they do there?
        Log("  first diff (%d,%d): real %d, composed %d, base %d", fx, fy, src[(size_t)fy * P + fx], g_cmp[(size_t)fy * P + fx], g_gnd[(size_t)fy * P + fx]); int shown = 0;
        for (size_t k = 0; k < g_rec.size() && shown < 8; k++) { const LeafRec& r = g_rec[k]; SprEntry2* e = GetSprite(r.id, r.a); if (!e || !e->w) continue; int bx = r.a[0] + e->ox, by = r.a[1] + e->oy;
            if (fx < bx || fx >= bx + e->w || fy < by || fy >= by + e->h) continue; size_t j = (size_t)(fy - by) * e->w + (fx - bx);
            Log("    rec #%zu leaf %X args %d %d %08X %08X %08X clip %d %d %d %d: kind %d val %d tid %d", k, kLeaf[r.id].addr, r.a[0], r.a[1], r.a[2], r.a[3], r.a[4], r.clip[0], r.clip[1], r.clip[2], r.clip[3], e->kind[j], e->val[j], e->tid[j]); shown++; } }
    static double lastLog = 0; double now = QpcMs();
    if (g_ss.frames <= 3 || now - lastLog > 5000) { lastLog = now;
        char line[900]; int n = _snprintf(line, sizeof line - 1, "sprites: frame %ld: %zu records, atlas %zu sprites (%ld probes, %ld failed, %.0f ms total, %zu tables); this frame %ld of %ld px differ (%.4f%%), no-sprite %ld; compare %.1f ms | total frames %ld, bad frames %ld",
            g_ss.frames, g_rec.size(), g_atlas.size(), g_probeCount, g_probeFail, g_probeMs, g_tbl.size(), diff, px, px ? 100.0 * diff / px : 0.0, noSpr, g_ss.ms / g_ss.frames, g_ss.frames, g_ss.diffFrames);
        line[n] = 0; Log("%s", line);
        if (diff) { n = 0; for (int i = 0; i < NLEAF; i++) if (perLeaf[i]) n += _snprintf(line + n, sizeof line - n - 1, " %X:%ld", kLeaf[i].addr, perLeaf[i]); line[n] = 0; Log("  diff by last blitter:%s ; first at (%d,%d) blitter %d", line, fx, fy, fl); } }
}
// ---------------------------------------------------------------- fog of war: reference port of 00475E19 / 004749B1 / 00475240 (the formula the GPU pass will use)
static int FogLevel(unsigned v) { v &= 0xFFFF; return v >= 0x5DC ? 5 : *(const BYTE*)(0x6F31B0 + (0x5DC - v)); }
static long g_fogPartial, g_fogFull;
static void FogCellRef(BYTE* buf, int P, int x, int y, int c0, int c1, int c2, int c3) {
    if (*(BYTE*)0x82B940) return;
    if (c0 <= 0x3F && c1 <= 0x3F && c2 <= 0x3F && c3 <= 0x3F) return;
    if (c0 >= 0x60 && c1 >= 0x60 && c2 >= 0x60 && c3 >= 0x60) g_fogFull++; else g_fogPartial++;
    BYTE* d = buf + (size_t)y * P + x;
    if (c0 >= 0x60 && c1 >= 0x60 && c2 >= 0x60 && c3 >= 0x60) { for (int r = 0; r < 16; r++) memset(d + (size_t)r * P, 0, 32); return; }
    int A = c0 << 16, B = c1 << 16, C = c2 << 16, D = c3 << 16; int dA = (C - A) >> 4, dd = (A + D - C - B) >> 9, step = (B - A) >> 5, left = A; const BYTE* lut = (const BYTE*)0x6F51B0;
    for (int r = 0; r < 16; r++) { int e = left; for (int u = 0; u < 32; u++) { BYTE* q = d + (size_t)r * P + u; unsigned idx = ((unsigned)e >> 8 & 0xFFFFFF00u) | *q; if (u < 4) idx = (unsigned)(((int)e >> 8) & ~0xFF) | *q; *q = lut[idx]; e += step; } step += dd; left += dA; }
}
static void FogRef(BYTE* buf, int P) {
    int cols = GI(0x1010570), rows = GI(0x1010574), ox = GI(0x1010544), oy = GI(0x1010548); const WORD* t = (const WORD*)0x6AF490;
    for (int r = 0; r < rows; r++) for (int c = 0; c < cols; c++) { int i = r * 128 + c; FogCellRef(buf, P, c * 32 + ox, r * 16 + oy, FogLevel(t[i]), FogLevel(t[i + 1]), FogLevel(t[i + 0x80]), FogLevel(t[i + 0x81])); }
}
extern "C" void __cdecl SpritesPostFog() {                                      // called from SnapshotWorld: the real frame now has the fog
    { int W = g_recW, H = g_recH, P = g_recPitch;
    if (g_jobBuild) {                                                                                  // fog data (valid only now) -> job complete -> publish for the presenter
        std::shared_ptr<GpuWorldJob> job = g_jobBuild; g_jobBuild.reset();
        if (job->ok && job->W == W && job->H == H && job->P == P) {
            job->fogOn = *(BYTE*)0x82B940 == 0; job->cols = GI(0x1010570); job->rows = GI(0x1010574); job->ox = GI(0x1010544); job->oy = GI(0x1010548);
            if (job->cols < 1 || job->rows < 1 || job->cols > 128 || job->rows > 128) job->fogOn = false;
            if (job->fogOn) { const WORD* t = (const WORD*)0x6AF490; job->cells.assign((size_t)128 * 128, 0);
                for (int r = 0; r < job->rows; r++) for (int c = 0; c < job->cols; c++) { int i = r * 128 + c; unsigned l0 = FogLevel(t[i]), l1 = FogLevel(t[i + 1]), l2 = FogLevel(t[i + 0x80]), l3 = FogLevel(t[i + 0x81]); job->cells[(size_t)r * 128 + c] = l0 | l1 << 8 | l2 << 16 | l3 << 24; }
                job->lut.assign((size_t)256 * 128, 0); memcpy(job->lut.data(), (const void*)0x6F51B0, 256 * 128); }
            job->seq = g_world.seq + 1; EnterCriticalSection(&g_gwCs); g_jobPub = job; LeaveCriticalSection(&g_gwCs);
        } }
    }
#ifdef REMASTER_DEV
    if (g_preFogReal.empty() || (int)g_preFogReal.size() != g_recPitch * g_recH) return;
    BYTE* src = GP(0x689088); if (src != g_recBuf) return; int W = g_recW, H = g_recH, P = g_recPitch;
    double t0 = QpcMs(); g_fogPartial = g_fogFull = 0; FogRef(g_preFogReal.data(), P); long diff = 0; int fx = -1, fy = -1;
    for (int y = 0; y < H; y++) { const BYTE* a = src + (size_t)y * P; const BYTE* b = g_preFogReal.data() + (size_t)y * P; if (memcmp(a, b, W) == 0) continue; for (int x = 0; x < W; x++) if (a[x] != b[x]) { if (!diff) { fx = x; fy = y; } diff++; } }
    static long n; static double lastLog; double now = QpcMs(); n++; if (n <= 3 || now - lastLog > 5000) { lastLog = now; Log("fogcheck: pre-fog frame + fog port: %ld of %d px differ from the real post-fog frame (%.4f%%), first at (%d,%d); fog cells: %ld partial, %ld fully dark, flag82B940=%d; %.1f ms", diff, W * H, 100.0 * diff / (W * H), fx, fy, g_fogPartial, g_fogFull, *(BYTE*)0x82B940, QpcMs() - t0); }
    g_preFogReal.clear(); g_preFogCmp.clear();
#endif
}
static void SpritesReset() {                                                     // mode change / new mission: sprite banks are reloaded, forget everything derived from the old ones
    if (!g_gwCs.DebugInfo) return;
    EnterCriticalSection(&g_gwCs);
    g_atlas.clear(); g_tbl.clear(); g_tblId.clear(); g_pend.clear(); g_pendSet.clear(); g_shelfX = g_shelfY = g_shelfH = 0; g_atlasFull = false; g_atlasDirtyY0 = 1 << 30; g_atlasDirtyY1 = -1; g_jobPub.reset(); g_jobBuild.reset(); g_rec.clear(); InterlockedExchange(&g_recOn, 0); InterlockedIncrement(&g_gwEpoch);
    LeaveCriticalSection(&g_gwCs);
}
static void __declspec(naked) SpritesPreFogThunk() {
    __asm { pushfd
            pushad
            call SpritesPreFog
            popad
            popfd
            push 0x4759E3
            ret }
}
#else
static void SpritesFrameBegin() {}
static void SpritesIdleWork(double) {}
static void SpritesReset() {}
#endif
