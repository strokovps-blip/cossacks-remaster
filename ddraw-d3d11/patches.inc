// Runtime patches of dmcr.exe hot paths (Cossacks: The Art of War v1.30, image base 0x400000, no ASLR).
// Every patch verifies the original bytes first; creating "patch.off" next to dmcr.exe disables all of them.
//
//  0046DFF5  CopyRect(x,y,w,h)   cdecl   offscreen buffer -> real (primary) screen. Re-implemented; also reports the dirty rect
//                                          to the presenter and counts frames (a full-screen copy == one game frame).
//  0046E450  PutImage(x,y)       thiscall  raw bitmap -> offscreen buffer, row by row. Re-implemented with plain memcpy.
//  00477E0F.. RLE sprite blitters: `rep movsd; mov cl,al; rep movsb` per run (startup-bound for short runs) -> SSE copy stub.

#define GI(a) (*(int*)(a))
#define GP(a) (*(BYTE**)(a))

#include "trace.inc"
#include "diag.inc"
#include "writewatch.inc"
#include "ground.inc"

static volatile LONG g_patchCopyActive;         // set once the CopyRect hook has been seen working
static volatile LONG g_gameFrames, g_dirtyEvents;
static CRITICAL_SECTION g_dirtyCs; static RECT g_rcDirty = {0, 0, 0, 0}; static bool g_rcValid;
static volatile LONG g_copyPtrMismatch;

static void AddDirty(int x, int y, int w, int h) {
    EnterCriticalSection(&g_dirtyCs);
    if (!g_rcValid) { g_rcDirty = {x, y, x + w, y + h}; g_rcValid = true; }
    else { if (x < g_rcDirty.left) g_rcDirty.left = x; if (y < g_rcDirty.top) g_rcDirty.top = y; if (x + w > g_rcDirty.right) g_rcDirty.right = x + w; if (y + h > g_rcDirty.bottom) g_rcDirty.bottom = y + h; }
    LeaveCriticalSection(&g_dirtyCs);
}
static bool TakeDirty(RECT* r) {
    EnterCriticalSection(&g_dirtyCs); bool v = g_rcValid; if (v) { *r = g_rcDirty; g_rcValid = false; } LeaveCriticalSection(&g_dirtyCs); return v;
}

#ifdef REMASTER_DEV
static bool g_diagOn = true;                       // developer build only (build_dev.bat): trace hooks active, never in the release DLL
#else
static bool g_diagOn;
#endif
//                              // master switch for everything diagnostic/experimental: create `diag.on` in the game folder
static void __cdecl My_CopyRect(int x, int y, int w, int h) {
    if (!GI(0x687FDC)) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > GI(0x6888A0)) w = GI(0x6888A0) - x;
    if (y + h > GI(0x68889C)) h = GI(0x68889C) - y;
    if (w < 0 || h < 0) return;
    int srcStride = GI(0x687FC8), dstStride = GI(0x687FD0);
    const BYTE* s = GP(0x689088) + x + y * srcStride; BYTE* d = GP(0x68908C) + x + y * dstStride;
    if (h == 0) return;
    if (w == srcStride && w == dstStride) memcpy(d, s, (size_t)w * h);
    else for (int r = 0; r < h; r++) { memcpy(d, s, w); s += srcStride; d += dstStride; }
    if (!g_patchCopyActive) {
        Surf* p = g_primary; Log("CopyRect hook active: real screen ptr=%p (primary mem=%p) offscreen=%p strides %d/%d", GP(0x68908C), p ? p->mem : nullptr, GP(0x689088), srcStride, dstStride);
        if (!p || GP(0x68908C) != p->mem) g_copyPtrMismatch = 1;
        g_patchCopyActive = 1;
    }
    AddDirty(x, y, w, h); InterlockedIncrement(&g_dirtyEvents);
    if (w >= GI(0x6888A0) && h >= GI(0x68889C)) { InterlockedIncrement(&g_gameFrames); InterlockedIncrement(&g_frameSeq); { LARGE_INTEGER f_, c_; QueryPerformanceFrequency(&f_); QueryPerformanceCounter(&c_); g_lastFrameMs = c_.QuadPart * 1000.0 / f_.QuadPart; LONG k_ = g_camN & 255; g_camRing[k_].t = g_lastFrameMs; g_camRing[k_].x = GI(0xAF5E00); g_camRing[k_].y = GI(0xAF5E04); g_camN = g_camN + 1; } if (g_diagOn) WWFrameEnd(); }
}

static void __fastcall My_PutImage(void* self, void* /*edx*/, int x, int y) {
    if (!GI(0x687FDC) || !self) return;
    const BYTE* img = *(const BYTE**)self; if (!img) return;
    int w = *(const WORD*)img, h = *(const WORD*)(img + 2);
    if (h <= 0 || w <= 0 || w > GI(0x6888A0) || h > GI(0x68889C)) return;
    int pitch = GI(0x689068); BYTE* d = GP(0x689088) + x + y * pitch; const BYTE* s = img + 4;
    for (int r = 0; r < h; r++) { memcpy(d, s, w); s += w; d += pitch; }
}

// SSE copy of n = ecx*4 + al bytes from esi to edi (advancing both), used instead of `rep movsd; mov cl,al; rep movsb`.
// Preserves eax, edx and all other registers except ecx (=0 on exit), esi, edi. No source over-read.
static void __declspec(naked) SpriteCopyStub() {
    __asm {
        push ebx
        movzx ebx, al
        lea ecx, [ebx + ecx*4]
        cmp ecx, 16
        jb L_small
        movdqu xmm1, [esi + ecx - 16]
        lea ebx, [edi + ecx - 16]
    L_big:
        movdqu xmm0, [esi]
        movdqu [edi], xmm0
        add esi, 16
        add edi, 16
        sub ecx, 16
        cmp ecx, 16
        ja L_big
        movdqu [ebx], xmm1
        add esi, ecx
        add edi, ecx
        jmp L_done
    L_small:
        cmp ecx, 8
        jb L_lt8
        movq xmm0, qword ptr [esi]
        movq xmm1, qword ptr [esi + ecx - 8]
        movq qword ptr [edi], xmm0
        movq qword ptr [edi + ecx - 8], xmm1
        add esi, ecx
        add edi, ecx
        jmp L_done
    L_lt8:
        cmp ecx, 4
        jb L_lt4
        movd xmm0, dword ptr [esi]
        movd xmm1, dword ptr [esi + ecx - 4]
        movd dword ptr [edi], xmm0
        movd dword ptr [edi + ecx - 4], xmm1
        add esi, ecx
        add edi, ecx
        jmp L_done
    L_lt4:
        test ecx, ecx
        jz L_done
        mov bl, byte ptr [esi]
        mov byte ptr [edi], bl
        cmp ecx, 1
        je L_adv
        movzx ebx, word ptr [esi + ecx - 2]
        mov word ptr [edi + ecx - 2], bx
    L_adv:
        add esi, ecx
        add edi, ecx
    L_done:
        pop ebx
        xor ecx, ecx
        ret
    }
}

#include "cursor.inc"
#include "trace_calls.inc"
#include "layers.inc"
#include "sprites.inc"
#include "hitch.inc"

static bool BytesEq(DWORD addr, const char* hex) {
    BYTE* p = (BYTE*)addr; for (; hex[0] && hex[1]; hex += 2, p++) { unsigned v; if (sscanf(hex, "%2x", &v) != 1) return false; if (*p != v) return false; } return true;
}
static bool WriteJmp(DWORD from, void* to, int totalLen) {
    DWORD old; if (!VirtualProtect((void*)from, totalLen, PAGE_EXECUTE_READWRITE, &old)) return false;
    BYTE* p = (BYTE*)from; p[0] = 0xE9; *(int*)(p + 1) = (int)((BYTE*)to - (p + 5)); for (int i = 5; i < totalLen; i++) p[i] = 0x90;
    VirtualProtect((void*)from, totalLen, old, &old); FlushInstructionCache(GetCurrentProcess(), (void*)from, totalLen); return true;
}
static bool PatchCall(DWORD at, void* to, int len) { // replace `len` bytes (>=5) with call to + nops
    DWORD old; if (!VirtualProtect((void*)at, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    BYTE* p = (BYTE*)at; p[0] = 0xE8; *(int*)(p + 1) = (int)((BYTE*)to - (p + 5)); for (int i = 5; i < len; i++) p[i] = 0x90;
    VirtualProtect((void*)at, len, old, &old); FlushInstructionCache(GetCurrentProcess(), (void*)at, len); return true;
}
// The game's frame limiter (function around 004CD1DC) is a busy-wait: `do { pump_messages(); } while (now - last < 2 * gameSpeed ms)`,
// with gameSpeed = dword [00708E04] (default 50, 0..100; lower = faster game). It burns ~95% of a CPU core while the real
// per-frame work is only a few ms. Here we sleep (up to 1 ms, wakes on any input/message) before each pump instead.
static double PumpNow() { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return c.QuadPart * 1000.0 / f.QuadPart; }
static int __cdecl My_PumpWait() {
    double t = PumpNow(); if (g_lastPumpExit > 0 && t - g_lastPumpExit > 4.0) { double w = t - g_lastPumpExit; InterlockedIncrement(&g_workTicks); if (w > 40) InterlockedIncrement(&g_workOver40); if (w > g_workMax) g_workMax = w; }   // a long gap between two waits = the game was working
    SpritesIdleWork(3.0);
    MsgWaitForMultipleObjectsEx(0, nullptr, 1, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    int r = ((int(__cdecl*)())0x484012)(); g_lastPumpExit = PumpNow(); return r;
}
static void ApplyPatches() {
    InitializeCriticalSection(&g_dirtyCs);
    if ((DWORD)GetModuleHandleA(nullptr) != 0x400000) { Log("patches skipped: unexpected image base"); return; }
    int ok = 0;
    if (BytesEq(0x46DFF5, "558BEC83EC28")) { if (WriteJmp(0x46DFF5, (void*)My_CopyRect, 6)) { ok++; Log("patched CopyRect @46DFF5"); } } else Log("CopyRect: prologue mismatch, skipped");
    if (BytesEq(0x46E450, "558BEC83EC20")) { if (WriteJmp(0x46E450, (void*)My_PutImage, 6)) { ok++; Log("patched PutImage @46E450"); } } else Log("PutImage: prologue mismatch, skipped");
    {
        InitializeCriticalSection(&g_cur.cs);
        if (BytesEq(0x4C10C3, "558BEC833D") && BytesEq(0x4C1376, "558BEC83EC10") && BytesEq(0x4C11FA, "558BEC83EC0C")) {
            WriteJmp(0x4C10C3, (void*)My_CursorPrepare, 5); WriteJmp(0x4C1376, (void*)My_CursorFinish, 6);
            DWORD old; VirtualProtect((void*)0x4C11FA, 1, PAGE_EXECUTE_READWRITE, &old); *(BYTE*)0x4C11FA = 0xC3; VirtualProtect((void*)0x4C11FA, 1, old, &old);
            ok += 3; Log("patched software cursor (overlay mode)");
        } else Log("cursor functions: prologue mismatch, overlay skipped");
    }
    if (g_diagOn && GetFileAttributesA("trace.off") == INVALID_FILE_ATTRIBUTES && BytesEq(0x47C1EF, "558BEC83EC4456")) {
        g_trace = (TraceRec*)VirtualAlloc(nullptr, sizeof(TraceRec) * TRACE_MAX, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (WriteJmp(0x47C1EF, (void*)DrawSpriteThunk, 6)) { ok++; Log("patched sprite dispatcher @47C1EF (trace hook, idle until trace.now)"); }
    }
    if (g_diagOn && GetFileAttributesA("trace.sites") != INVALID_FILE_ATTRIBUTES) InstallCallTracing();
    {
        BYTE* b = (BYTE*)0x4E2E57;                                  // `call 0044B601`: first interface call; reached with or without the fog/terrain passes (they are skipped when the whole map is open)
        if (b[0] == 0xE8 && (DWORD)(0x4E2E57 + 5 + *(int*)(b + 1)) == 0x44B601) {
            DWORD old; VirtualProtect(b, 5, PAGE_EXECUTE_READWRITE, &old); *(int*)(b + 1) = (int)((BYTE*)SnapshotThunk - (b + 5)); VirtualProtect(b, 5, old, &old); ok++; Log("patched world/interface layer split @4E2E57");
        } else Log("layer split site mismatch, skipped");
    }
#ifdef REMASTER_GPUW
    if (true) {                                                 // the ground blit call is the start of the world phase (frame begin for the sprite list)
#else
    if (g_diagOn && GetFileAttributesA("ground.off") == INVALID_FILE_ATTRIBUTES) {
#endif
        BYTE* b = (BYTE*)0x445796;
        if (b[0] == 0xE8 && (DWORD)(0x445796 + 5 + *(int*)(b + 1)) == 0x4C44F0) { DWORD old; VirtualProtect(b, 5, PAGE_EXECUTE_READWRITE, &old); *(int*)(b + 1) = (int)((BYTE*)My_Ground - (b + 5)); VirtualProtect(b, 5, old, &old); ok++; Log("patched ground refill+blit call @445796 (cache -> GPU data)"); }
        else Log("ground call site mismatch, skipped");
    }
    static const DWORD sites[] = {0x477FAC, 0x477FF6, 0x4780BD, 0x47813C, 0x47820E, 0x478293};
#ifndef NO_SPRITESTUB
    for (DWORD a : sites) if (BytesEq(a, "F3A58AC8F3A4")) { if (PatchCall(a, (void*)SpriteCopyStub, 6)) ok++; } else Log("sprite site %X mismatch, skipped", a);
#endif
    {
        if (BytesEq(0x4CD1DC, "E8316EFBFF")) { if (PatchCall(0x4CD1DC, (void*)My_PumpWait, 5)) { ok++; Log("patched frame-limiter busy-wait @4CD1DC (sleeps 1 ms per iteration)"); } }
        else Log("frame limiter call site mismatch, skipped");
    }
#ifdef REMASTER_GPUW
    InstallLeafHooks(ok);
#ifdef REMASTER_DEV
    InstallProbeHooks(ok);
#endif
    {
        BYTE* b = (BYTE*)0x4E2E3D;                                  // `call 004759E3`: the fog passes start here, the world without fog is in the buffer
        if (b[0] == 0xE8 && (DWORD)(0x4E2E3D + 5 + *(int*)(b + 1)) == 0x4759E3) { DWORD old; VirtualProtect(b, 5, PAGE_EXECUTE_READWRITE, &old); *(int*)(b + 1) = (int)((BYTE*)SpritesPreFogThunk - (b + 5)); VirtualProtect(b, 5, old, &old); ok++; Log("patched pre-fog hook @4E2E3D"); }
        else Log("pre-fog site mismatch, skipped");
    }
#endif
    Log("applied %d patches", ok);
}
