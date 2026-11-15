// ---------------------------------------------------------------- call-site tracing of the blitter family (0x4711AE..0x47E000)
// Enabled only if "trace.sites" exists next to dmcr.exe at start. Every direct `call <blitter>` site is redirected to a
// per-target logging thunk (no function prologue has to be decoded). With "trace.now" the next 3 frames are recorded into
// trace_calls.txt: the ORDER in which the game draws a frame (terrain, objects, units, interface), with arguments.
#include "trace_sites.inc"
struct CallRec { DWORD frame, target, ret, ecx, a0, a1, a2, a3; };
#define CALLS_MAX 400000
static CallRec* g_calls;

extern "C" void __cdecl TraceCallSite(DWORD id, DWORD* retp, DWORD ecx) {
    LONG f = g_frameSeq;
    if (f < g_traceFrom || f >= g_traceTo || !g_calls) return;
    LONG i = InterlockedIncrement(&g_callsN) - 1; if (i >= CALLS_MAX) return;
    CallRec& r = g_calls[i]; r.frame = f; r.target = kTraceTargets[id]; r.ret = retp[0]; r.ecx = ecx; r.a0 = retp[1]; r.a1 = retp[2]; r.a2 = retp[3]; r.a3 = retp[4];
}
static void InstallCallTracing() {
    g_calls = (CallRec*)VirtualAlloc(nullptr, sizeof(CallRec) * CALLS_MAX, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    const int nT = sizeof(kTraceTargets) / sizeof(kTraceTargets[0]);
    BYTE* mem = (BYTE*)VirtualAlloc(nullptr, nT * 48, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    for (int id = 0; id < nT; id++) {
        BYTE* q = mem + id * 48; BYTE* p = q;
        *p++ = 0x60; *p++ = 0x9C;                                            // pushad; pushfd
        *p++ = 0x8D; *p++ = 0x44; *p++ = 0x24; *p++ = 0x24;                  // lea eax,[esp+36]
        *p++ = 0x51; *p++ = 0x50;                                            // push ecx; push eax
        *p++ = 0x68; *(int*)p = id; p += 4;                                  // push id
        *p++ = 0xB9; *(void**)p = (void*)&TraceCallSite; p += 4;             // mov ecx, TraceCallSite
        *p++ = 0xFF; *p++ = 0xD1;                                            // call ecx
        *p++ = 0x83; *p++ = 0xC4; *p++ = 0x0C;                               // add esp,12
        *p++ = 0x9D; *p++ = 0x61;                                            // popfd; popad
        *p++ = 0xE9; *(int*)p = (int)(kTraceTargets[id] - (DWORD)(p + 4)); p += 4;   // jmp target
    }
    int patched = 0, skipped = 0;
    for (auto& s : kTraceSites) {
        DWORD at = s[0], target = s[1]; BYTE* b = (BYTE*)at;
        if (b[0] != 0xE8 || (DWORD)(at + 5 + *(int*)(b + 1)) != target) { skipped++; continue; }
        int id = -1; for (int k = 0; k < nT; k++) if (kTraceTargets[k] == target) { id = k; break; }
        DWORD old; VirtualProtect(b, 5, PAGE_EXECUTE_READWRITE, &old); *(int*)(b + 1) = (int)((DWORD)(mem + id * 48) - (at + 5)); VirtualProtect(b, 5, old, &old); patched++;
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    Log("call-site tracing installed: %d sites patched, %d skipped, %d targets", patched, skipped, nT);
}

// ---------------------------------------------------------------- RLE sprite decoder (format of blitter 00477E0F)
// sprite record P:  +4 int16 dx, +6 int16 dy, +8 int16 width, +0x15 int16 height, +0x17 row stream.
// pixels are a separate stream at (4th argument + [005D2800]).  Row stream: byte n; n==0: empty row;
//   n&0x80 == 0: n runs of (skip byte, length byte);  n&0x80: runs = n&0x1F, each ONE byte: skip = (b&15)|(n&0x40?16:0), length = (b>>4)|(n&0x20?16:0).
struct Spr { int w = 0, h = 0, dx = 0, dy = 0; std::vector<u8> idx, mask; };
static bool DecodeSpr(DWORD P, DWORD pixArg, Spr& s) {
    if (IsBadReadPtr((void*)P, 0x20)) return false;
    const BYTE* hd = (const BYTE*)P; s.dx = *(short*)(hd + 4); s.dy = *(short*)(hd + 6); s.w = *(short*)(hd + 8); s.h = *(short*)(hd + 0x15);
    if (s.w <= 0 || s.h <= 0 || s.w > 512 || s.h > 512) return false;
    const BYTE* rle = hd + 0x17; const BYTE* pix = (const BYTE*)(pixArg + GI(0x5D2800));
    if (IsBadReadPtr(pix, 16)) return false;
    s.idx.assign((size_t)s.w * s.h, 0); s.mask.assign((size_t)s.w * s.h, 0);
    for (int y = 0; y < s.h; y++) {
        if (IsBadReadPtr(rle, 4)) return false; int n = *rle++, x = 0; if (n == 0) continue;
        int runs, f1 = 0, f2 = 0; bool packed = (n & 0x80) != 0;
        if (packed) { runs = n & 0x1F; f1 = (n & 0x40) ? 16 : 0; f2 = (n & 0x20) ? 16 : 0; } else runs = n;
        for (int r = 0; r < runs; r++) {
            int skip, len; if (packed) { int b = *rle++; skip = (b & 15) | f1; len = (b >> 4) | f2; } else { skip = rle[0]; len = rle[1]; rle += 2; }
            x += skip; if (x + len > s.w) len = s.w - x < 0 ? 0 : s.w - x;
            for (int k = 0; k < len; k++) { s.idx[(size_t)y * s.w + x + k] = pix[k]; s.mask[(size_t)y * s.w + x + k] = 1; }
            pix += (packed ? ((0)) : 0) + 0; pix += len; x += len;
        }
    }
    return true;
}
static void DumpSprites() {
    Pal* pal = g_lastPal; std::map<DWORD, bool> seen; int n = 0; DWORD mid = g_traceFrom + 1;
    for (LONG i = 0; i < min((LONG)g_callsN, (LONG)CALLS_MAX) && n < 12; i++) {
        CallRec& r = g_calls[i]; if (r.frame != mid || r.target != 0x477E0F) continue; if (seen[r.a2]) continue; seen[r.a2] = true;
        Spr sp; if (!DecodeSpr(r.a2, r.a3, sp)) continue; if (sp.w < 8 || sp.h < 8) continue;
        std::vector<u8> rgb((size_t)sp.w * sp.h * 3);
        for (int k = 0; k < sp.w * sp.h; k++) { u8* d = &rgb[(size_t)k * 3]; if (sp.mask[k]) { PALETTEENTRY e = pal ? pal->e[sp.idx[k]] : PALETTEENTRY{sp.idx[k], sp.idx[k], sp.idx[k], 0}; d[0] = e.peRed; d[1] = e.peGreen; d[2] = e.peBlue; } else { d[0] = 255; d[1] = 0; d[2] = 255; } }
        char fn[64]; sprintf(fn, "sprite%02d_%dx%d_at%08X.png", n++, sp.w, sp.h, r.a2); WritePng(fn, sp.w, sp.h, rgb.data());
        Log("decoded sprite %s (dx %d dy %d)", fn, sp.dx, sp.dy);
    }
}
static void CallsDump() {
    FILE* f = fopen("trace_calls.txt", "w"); if (!f) return;
    LONG n = min((LONG)g_callsN, (LONG)CALLS_MAX);
    DWORD mid = g_traceFrom + 1;                                              // write the middle frame in order, run-length encoded by (target,caller)
    fprintf(f, "# %ld blitter calls captured over frames %ld..%ld; ordered listing of frame %lu\n# target caller this(ecx) a0 a1 a2 a3   (xN = repeated N times)\n", n, g_traceFrom, g_traceTo - 1, mid);
    std::map<DWORD, int> perFrame; for (LONG i = 0; i < n; i++) perFrame[g_calls[i].frame]++;
    fprintf(f, "# calls per frame:"); for (auto& kv : perFrame) fprintf(f, " %lu:%d", kv.first, kv.second); fputc('\n', f);
    LONG i = 0; while (i < n && g_calls[i].frame != mid) i++;
    while (i < n && g_calls[i].frame == mid) {
        CallRec& r = g_calls[i]; LONG j = i + 1; while (j < n && g_calls[j].frame == mid && g_calls[j].target == r.target && g_calls[j].ret == r.ret) j++;
        fprintf(f, "%08X <- %08X  this=%08X  %d %d %08X %08X", r.target, r.ret, r.ecx, (int)r.a0, (int)r.a1, r.a2, r.a3); if (j - i > 1) fprintf(f, "  x%ld  (last: %d %d)", j - i, (int)g_calls[j - 1].a0, (int)g_calls[j - 1].a1); fputc('\n', f);
        i = j;
    }
    fclose(f);
    std::map<std::pair<DWORD, DWORD>, int> byPair; for (LONG k = 0; k < n; k++) if (g_calls[k].frame == mid) byPair[{g_calls[k].target, g_calls[k].ret}]++;
    FILE* s = fopen("trace_calls_summary.txt", "w"); if (s) { fprintf(s, "frame %lu: blitter target <- caller : calls\n", mid); for (auto& kv : byPair) fprintf(s, "  %08X <- %08X : %d\n", kv.first.first, kv.first.second, kv.second); fclose(s); }
    Log("call trace dumped: %ld calls", n); DumpSprites();
}
