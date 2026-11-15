// ---------------------------------------------------------------- software cursor -> GPU overlay
// The game draws its cursor into the frame only when its main loop runs (~21 Hz in a mission), so it judders.
// We stop the game from drawing it, render the cursor sprite once into a scratch buffer (using the game's own sprite blitter),
// and let the presenter draw it at the live OS cursor position on every vsync.
//   004C10C3  cursor prepare (position/hotspot, restore background, draw sprite into the offscreen frame)  -> My_CursorPrepare
//   004C1376  cursor update on the real screen after CopyRect                                              -> My_CursorFinish
//   004C11FA  draw cursor on the real screen                                                               -> ret
#define CUR_W 128
#define CUR_H 128
#define CUR_ORG 48                       // sprite is rendered at (CUR_ORG, CUR_ORG) inside the scratch buffer
struct CursorOverlay { CRITICAL_SECTION cs; volatile LONG version; bool visible; int hotX, hotY; BYTE img[CUR_W * CUR_H]; BYTE mask[CUR_W * CUR_H]; } g_cur;
static volatile LONG g_cursorHooked;
static int g_curFrame = -1, g_curShape = -1;

typedef void(__fastcall* SpriteFn)(void* self, void* edx, int x, int y, int frame, int arg4, int arg5);
static void RenderCursorScratch(BYTE* out, BYTE fill) {
    memset(out, fill, CUR_W * CUR_H);
    int s0 = GI(0x689088), s1 = GI(0x689068), s2 = GI(0x689078), s3 = GI(0x68907C), s4 = GI(0x689080), s5 = GI(0x689084);
    GI(0x689088) = (int)out; GI(0x689068) = CUR_W; GI(0x689080) = 0; GI(0x689084) = 0; GI(0x689078) = CUR_W - 1; GI(0x68907C) = CUR_H - 1;
    ((SpriteFn)0x47C1EF)((void*)0x703AE0, nullptr, CUR_ORG, CUR_ORG, GI(0x248BE98), GI(0x70E6A0), 0);
    GI(0x689088) = s0; GI(0x689068) = s1; GI(0x689078) = s2; GI(0x68907C) = s3; GI(0x689080) = s4; GI(0x689084) = s5;
}
static void __cdecl My_CursorPrepare() {                 // replaces 004C10C3
    if (!GI(0x687FDC)) return;
    if (GI(0x70E6A0) == 8) { GI(0x70D684) = 0x10; GI(0x70D688) = 0x11; } else { GI(0x70D684) = 5; GI(0x70D688) = 5; }
    if (*(BYTE*)0x82B5A9) { g_cur.visible = false; return; }
    *(BYTE*)0x70D680 = 1;
    GI(0x70D68C) = GI(0x70D678) - GI(0x70D684); GI(0x70D690) = GI(0x70D67C) - GI(0x70D688);
    int frame = GI(0x248BE98), shape = GI(0x70E6A0);
    if (frame != g_curFrame || shape != g_curShape || !g_cur.version) {
        static BYTE a[CUR_W * CUR_H], b[CUR_W * CUR_H]; RenderCursorScratch(a, 0); RenderCursorScratch(b, 255);
        EnterCriticalSection(&g_cur.cs);
        for (int i = 0; i < CUR_W * CUR_H; i++) { g_cur.mask[i] = (a[i] == b[i]) ? 255 : 0; g_cur.img[i] = a[i]; }
        InterlockedIncrement(&g_cur.version);
        LeaveCriticalSection(&g_cur.cs); g_curFrame = frame; g_curShape = shape;
    }
    g_cur.hotX = GI(0x70D684); g_cur.hotY = GI(0x70D688); g_cur.visible = true;
    if (!g_cursorHooked) { g_cursorHooked = 1; Log("cursor overlay active (frame %d shape %d hot %d,%d)", frame, shape, g_cur.hotX, g_cur.hotY); }
}
static void __cdecl My_CursorFinish() {                  // replaces 004C1376: nothing is drawn any more, only keep the bookkeeping
    if (*(BYTE*)0x82B5A9) return;
    GI(0x70D670) = GI(0x70D68C); GI(0x70D674) = GI(0x70D690); *(BYTE*)0x70D680 = 0;
}
