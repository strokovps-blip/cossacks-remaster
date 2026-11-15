// ---------------------------------------------------------------- world / interface layer split
// 004E1A66 draws a whole frame in this order: objects, terrain passes (004E2E3D / 004E2E42), then ALL interface.
// Right after the terrain passes (the instruction at 004E2E47 is `call 0054294E`) the offscreen buffer holds the pure world.
// We redirect that call to a thunk that snapshots the buffer and then tail-jumps to the original target.
//   world layer  = snapshot at that moment
//   interface    = pixels where the final frame differs from the snapshot (exact, any window/menu/minimap)
struct WorldLayer { u8* buf = nullptr; size_t sz = 0; int w = 0, h = 0, pitch = 0; volatile LONG seq = 0; double tSnap = 0; bool ok = false; int camX = 0, camY = 0; } g_world;
static volatile LONG g_layerHits; static LONG LayerHits() { return g_layerHits; }
static double QpcMs() { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return c.QuadPart * 1000.0 / f.QuadPart; }

#ifdef REMASTER_GPUW
extern "C" void __cdecl SpritesPostFog();
#endif
extern "C" void __cdecl SnapshotWorld() {
#ifdef REMASTER_GPUW
    SpritesPostFog();
#endif
    BYTE* src = GP(0x689088); int pitch = GI(0x687FC8), w = GI(0x6888A0), h = GI(0x68889C);
    if (!src || pitch <= 0 || w <= 0 || h <= 0) return;
    size_t sz = (size_t)pitch * h;
    if (g_world.sz != sz) { g_world.buf = (u8*)realloc(g_world.buf, sz); g_world.sz = sz; }
    g_world.w = w; g_world.h = h; g_world.pitch = pitch; memcpy(g_world.buf, src, sz); g_world.camX = GI(0xAF5E00); g_world.camY = GI(0xAF5E04); g_world.ok = true; InterlockedIncrement(&g_world.seq); if (InterlockedIncrement(&g_layerHits) == 1) Log("world layer snapshot active: %dx%d pitch %d", w, h, pitch);
}
static void __declspec(naked) SnapshotThunk() {
    __asm {
        pushfd
        pushad
        call SnapshotWorld
        popad
        popfd
        push 0x44B601                // tail-jump to the original callee of the patched `call` (keeps all registers)
        ret
    }
}
