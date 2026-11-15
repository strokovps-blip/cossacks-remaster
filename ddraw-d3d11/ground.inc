// ---------------------------------------------------------------- ground (terrain) cache -> data for the GPU
// The game never draws terrain per frame: a pre-rendered 8-bit ring-buffer cache (object at 0070F480; width [obj], height [obj+4], pixels at [obj+4Dh])
// is refilled in strips while scrolling, and 004C2A00 copies the visible window out of it with wrap-around into the offscreen buffer (004C28C0 x4):
//   srcX = (camX*32) mod W,  srcY = (2*camY*8) mod H,  window = ([01010570]*32) x ([01010574]*16) pixels, written to the offscreen buffer at
//   ([01010544],[01010548]) (sub-cell offsets, normally (0,8)).
// This hook (call site 00445796 -> 004C44F0 = refill + blit) lets the GPU path read the same data. "ground.check" in the game folder asks for a one-off self-test:
// the window is rebuilt from the cache here and compared with what the game just wrote into the offscreen buffer.
struct GroundInfo { volatile LONG seq; int W, H; const BYTE* pix; int srcX, srcY, dstX, dstY, winW, winH; };
static GroundInfo g_ground;

static void SpritesFrameBegin();
static void __fastcall My_Ground(void* self, void* /*edx*/) {
    ((void(__fastcall*)(void*, void*))0x4C44F0)(self, nullptr);                         // original: refill strips + blit
    SpritesFrameBegin();
    GroundInfo g; g.W = *(int*)self; g.H = *((int*)self + 1); g.pix = *(const BYTE**)((BYTE*)self + 0x4D);
    int camX = GI(0xAF5E00), camY = GI(0xAF5E04); if (g.W <= 0 || g.H <= 0 || !g.pix) return;
    g.srcX = (int)(((long long)camX * 32) % g.W); if (g.srcX < 0) g.srcX += g.W;
    g.srcY = (int)(((long long)camY * 16) % g.H); if (g.srcY < 0) g.srcY += g.H;
    g.dstX = GI(0x1010544); g.dstY = GI(0x1010548); g.winW = GI(0x1010570) * 32; g.winH = GI(0x1010574) * 16; g.seq = 0;
    static bool logged; if (!logged) { logged = true; Log("ground cache: %dx%d pix=%p, window %dx%d, src (%d,%d), dst (%d,%d), cam (%d,%d)", g.W, g.H, g.pix, g.winW, g.winH, g.srcX, g.srcY, g.dstX, g.dstY, camX, camY); }
    g_ground = g; g_ground.seq = (LONG)g_frameSeq + 1;
#ifdef REMASTER_DEV
    static DWORD lastChk; DWORD tk = GetTickCount();
    if (tk - lastChk >= 1000) { lastChk = tk; if (GetFileAttributesA("ground.check") != INVALID_FILE_ATTRIBUTES) {
        DeleteFileA("ground.check");
        const BYTE* buf = GP(0x689088); int pitch = GI(0x687FC8), sw = GI(0x6888A0), sh = GI(0x68889C); long bad = 0, tot = 0; int firstX = -1, firstY = -1;
        for (int y = 0; y < g.winH && g.dstY + y < sh; y++) for (int x = 0; x < g.winW && g.dstX + x < sw; x++) {
            if (g.dstX + x < 0 || g.dstY + y < 0) continue;
            BYTE e = g.pix[(size_t)((g.srcY + y) % g.H) * g.W + (g.srcX + x) % g.W], a = buf[(size_t)(g.dstY + y) * pitch + g.dstX + x]; tot++;
            if (e != a) { if (!bad) { firstX = x; firstY = y; } bad++; } }
        Log("ground self-test: %ld of %ld window pixels differ from the cache model (first at %d,%d)", bad, tot, firstX, firstY); } }
#endif
}
