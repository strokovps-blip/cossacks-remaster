// Own implementation of DirectDraw (IDirectDraw 1/2/4/7 + surfaces/palettes/clippers) for x86.
// Surfaces live in system memory; the primary surface is presented by a background thread.
// Phase 1: GDI presenter (StretchDIBits). Phase 2: replace Present() with D3D11.
#define WIN32_LEAN_AND_MEAN
#if defined(REMASTER_DEV) && !defined(REMASTER_GPUW)
#define REMASTER_GPUW                    // the developer build contains the GPU world compositor too
#endif
#include <windows.h>
#include <objbase.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <mmsystem.h>
#include <math.h>
#include <vector>
#include <string>
#include <algorithm>
#include <map>

typedef unsigned char u8; typedef unsigned int u32;

enum Kind { DD1, DD2, DD4, DD7, S1, S2, S3, S4, S7, PAL, CLIP, KINDS };
static const int kCount[KINDS] = {23, 24, 28, 30, 36, 39, 40, 45, 49, 7, 9};
static const char* kName[KINDS] = {"DD1","DD2","DD4","DD7","S1","S2","S3","S4","S7","PAL","CLIP"};
static bool IsDD(Kind k) { return k <= DD7; }
static bool IsSurf(Kind k) { return k >= S1 && k <= S7; }

static const char* ddNames[] = {"QueryInterface","AddRef","Release","Compact","CreateClipper","CreatePalette","CreateSurface","DuplicateSurface","EnumDisplayModes","EnumSurfaces","FlipToGDISurface","GetCaps","GetDisplayMode","GetFourCCCodes","GetGDISurface","GetMonitorFrequency","GetScanLine","GetVerticalBlankStatus","Initialize","RestoreDisplayMode","SetCooperativeLevel","SetDisplayMode","WaitForVerticalBlank","GetAvailableVidMem","GetSurfaceFromDC","RestoreAllSurfaces","TestCooperativeLevel","GetDeviceIdentifier","StartModeTest","EvaluateMode"};
static const char* sNames[] = {"QueryInterface","AddRef","Release","AddAttachedSurface","AddOverlayDirtyRect","Blt","BltBatch","BltFast","DeleteAttachedSurface","EnumAttachedSurfaces","EnumOverlayZOrders","Flip","GetAttachedSurface","GetBltStatus","GetCaps","GetClipper","GetColorKey","GetDC","GetFlipStatus","GetOverlayPosition","GetPalette","GetPixelFormat","GetSurfaceDesc","Initialize","IsLost","Lock","ReleaseDC","Restore","SetClipper","SetColorKey","SetOverlayPosition","SetPalette","Unlock","UpdateOverlay","UpdateOverlayDisplay","UpdateOverlayZOrder","GetDDInterface","PageLock","PageUnlock","SetSurfaceDesc","SetPrivateData","GetPrivateData","FreePrivateData","GetUniquenessValue","ChangeUniquenessValue","SetPriority","GetPriority","SetLOD","GetLOD"};
static const char* palNames[] = {"QueryInterface","AddRef","Release","GetCaps","GetEntries","Initialize","SetEntries"};
static const char* clipNames[] = {"QueryInterface","AddRef","Release","GetClipList","GetHWnd","Initialize","IsClipListChanged","SetClipList","SetHWnd"};
static const char* NameOf(Kind k, int i) {
    if (IsDD(k)) return ddNames[i];
    if (IsSurf(k)) return sNames[i];
    return k == PAL ? palNames[i] : clipNames[i];
}
// stdcall argument counts (including `this`) for the generic "not implemented" stubs
static const int ddArgs[] = {3,1,1,1,4,5,4,3,5,5,1,3,2,3,2,2,2,2,2,1,3,6,3, 4, 3,1,1,3, 3,3};
static const int sArgs[]  = {3,1,1,2,2,6,4,6,3,3,4,3,3,2,2,2,3,2,2,3,2,2,2,3,1,5,2,1,2,3,3,2,2,6,3,3, 2,2,2, 3, 5,4,2,2,1, 2,2,2,2};
static const int palArgs[] = {3,1,1,2,5,4,5};
static const int clipArgs[] = {3,1,1,4,2,3,2,3,3};
static int ArgsOf(Kind k, int i) {
    if (IsDD(k)) return ddArgs[i];
    if (IsSurf(k)) return sArgs[i];
    return k == PAL ? palArgs[i] : clipArgs[i];
}

#define E_NOTIMPL_ ((HRESULT)0x80004001)
#define DD_OK 0
#define DDERR_GENERIC ((HRESULT)0x80004005)
#define DDERR_INVALIDPARAMS ((HRESULT)0x80070057)
#define DDERR_NOTFOUND ((HRESULT)0x887600FF)
#define DDERR_SURFACELOST ((HRESULT)0x887601C2)

// ---------------------------------------------------------------- logging
static FILE* g_log; static CRITICAL_SECTION g_cs;
#ifdef REMASTER_DEV
static void Log(const char* fmt, ...) {
    if (!g_log) return;
    EnterCriticalSection(&g_cs);
    va_list a; va_start(a, fmt);
    fprintf(g_log, "[%6lu] ", GetTickCount() & 0xFFFFFF);
    vfprintf(g_log, fmt, a); fputc('\n', g_log); fflush(g_log);
    va_end(a); LeaveCriticalSection(&g_cs);
}
#else
#define Log(...) ((void)0)                                  // release build: no log file, no diagnostics
#endif
static unsigned long g_cnt[KINDS][64];
static bool Throttle(Kind k, int i) { unsigned long n = ++g_cnt[k][i]; return n <= 12 || !(n & (n - 1)); }
#ifdef REMASTER_DEV
#define TRACE(k, i, ...) do { if (Throttle(k, i)) { char _b[300]; _snprintf(_b, sizeof _b, __VA_ARGS__); Log("%s::%s #%lu %s", kName[k], NameOf(k, i), g_cnt[k][i], _b); } } while (0)
#else
#define TRACE(k, i, ...) ((void)0)
#endif

// ---------------------------------------------------------------- object model
struct Impl; struct Obj { void** vtbl; Kind kind; Impl* impl; };
enum { T_DD, T_SURF, T_PAL, T_CLIP };
struct Impl { int type; LONG refs; Obj* faces[KINDS]; };
struct PixFmt { DWORD flags, rgbBits, rMask, gMask, bMask, aMask; };
struct Pal : Impl { PALETTEENTRY e[256]; bool allow256; };
// DirectDraw: without DDPCAPS_ALLOW256 the palette entries 0 and 255 are the reserved system colours (black and white), whatever the application supplies
static void PalReserve(Pal* p) { if (p->allow256) return; p->e[0].peRed = p->e[0].peGreen = p->e[0].peBlue = 0; p->e[255].peRed = p->e[255].peGreen = p->e[255].peBlue = 255; }
struct Clip : Impl { HWND hwnd; };
struct DD : Impl { int w = 640, h = 480, bpp = 8; HWND hwnd = nullptr; DWORD coop = 0; struct Surf* primary = nullptr; };
struct Surf : Impl {
    DD* dd; int w, h, bpp, pitch; u8* mem; DWORD caps; bool primary;
    Surf* back; Surf* front; Pal* pal; Clip* clip;
    DWORD ckLow, ckHigh; bool hasCk; PixFmt pf; int locks;
};
static void* g_vt[KINDS][64];
static void* g_h[KINDS][64];

static Obj* Face(Impl* im, Kind k) {
    if (!im->faces[k]) { Obj* o = new Obj; o->vtbl = g_vt[k]; o->kind = k; o->impl = im; im->faces[k] = o; }
    return im->faces[k];
}
static Impl* I(void* self) { return ((Obj*)self)->impl; }
static Kind K(void* self) { return ((Obj*)self)->kind; }
static Surf* S(void* self) { return (Surf*)((Obj*)self)->impl; }
static DD* D(void* self) { return (DD*)((Obj*)self)->impl; }

static Pal* g_lastPal;      // fallback palette for 8-bit presentation
static Surf* g_primary;     // current primary surface

// ---------------------------------------------------------------- pixel helpers
static void FillPF(PixFmt& pf, int bpp) {
    memset(&pf, 0, sizeof pf); pf.rgbBits = bpp;
    if (bpp == 8) pf.flags = 0x20 | 0x40;       // PALETTEINDEXED8|RGB
    else { pf.flags = 0x40; if (bpp == 16) { pf.rMask = 0xF800; pf.gMask = 0x07E0; pf.bMask = 0x001F; } else { pf.rMask = 0xFF0000; pf.gMask = 0xFF00; pf.bMask = 0xFF; } }
}
static void PutPF(BYTE* d, const PixFmt& pf) { // DDPIXELFORMAT at d
    DWORD* p = (DWORD*)d; p[0] = 32; p[1] = pf.flags; p[2] = 0; p[3] = pf.rgbBits; p[4] = pf.rMask; p[5] = pf.gMask; p[6] = pf.bMask; p[7] = pf.aMask;
}
static void FillDesc(Surf* s, BYTE* d, bool withPtr) {
    DWORD sz = *(DWORD*)d; memset(d, 0, sz); *(DWORD*)d = sz;
    DWORD* p = (DWORD*)d;
    p[1] = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000; // CAPS|HEIGHT|WIDTH|PITCH|PIXELFORMAT
    p[2] = s->h; p[3] = s->w; p[4] = s->pitch;
    if (withPtr) { p[1] |= 0x800; p[9] = (DWORD)s->mem; }
    PutPF(d + 72, s->pf);
    *(DWORD*)(d + 104) = s->caps;
}
static Surf* NewSurf(DD* dd, int w, int h, int bpp, const PixFmt* pf, DWORD caps) {
    Surf* s = new Surf(); memset((Impl*)s, 0, sizeof(Impl)); s->type = T_SURF; s->refs = 0;
    s->dd = dd; s->w = w; s->h = h; s->bpp = bpp; s->caps = caps;
    s->pitch = (w * ((bpp + 7) / 8) + 3) & ~3;
    s->mem = (u8*)VirtualAlloc(nullptr, (size_t)s->pitch * h + 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (pf) s->pf = *pf; else FillPF(s->pf, bpp);
    return s;
}

// ---------------------------------------------------------------- presenter (GDI, phase 1)
static HWND g_wnd0; // forward
static void GetDest(HWND wnd, int gw, int gh, int& ox, int& oy, int& dw, int& dh) {
    RECT rc; GetClientRect(wnd, &rc); int cw = rc.right, ch = rc.bottom; if (gw <= 0 || gh <= 0) { ox = oy = 0; dw = cw; dh = ch; return; }
    double sc = min((double)cw / gw, (double)ch / gh); dw = (int)(gw * sc); dh = (int)(gh * sc); ox = (cw - dw) / 2; oy = (ch - dh) / 2;
}
static volatile LONG g_dirty; static HWND g_presentWnd; static DWORD* g_bgra; static int g_bgraW, g_bgraH;
static void Present() {
    Surf* p = g_primary; if (!p || !p->mem || !g_presentWnd) return;
    if (g_bgraW != p->w || g_bgraH != p->h) { delete[] g_bgra; g_bgra = new DWORD[p->w * p->h]; g_bgraW = p->w; g_bgraH = p->h; }
    Pal* pal = p->pal ? p->pal : g_lastPal;
    for (int y = 0; y < p->h; y++) {
        const u8* src = p->mem + (size_t)y * p->pitch; DWORD* dst = g_bgra + y * p->w;
        if (p->bpp == 8) {
            for (int x = 0; x < p->w; x++) { PALETTEENTRY e = pal ? pal->e[src[x]] : PALETTEENTRY{src[x], src[x], src[x], 0}; dst[x] = (e.peRed << 16) | (e.peGreen << 8) | e.peBlue; }
        } else if (p->bpp == 16) {
            const WORD* s16 = (const WORD*)src; bool is565 = p->pf.gMask == 0x07E0;
            for (int x = 0; x < p->w; x++) { WORD v = s16[x]; u32 r, g, b;
                if (is565) { r = (v >> 11) & 31; g = (v >> 5) & 63; b = v & 31; r = r * 255 / 31; g = g * 255 / 63; b = b * 255 / 31; }
                else { r = (v >> 10) & 31; g = (v >> 5) & 31; b = v & 31; r = r * 255 / 31; g = g * 255 / 31; b = b * 255 / 31; }
                dst[x] = (r << 16) | (g << 8) | b; }
        } else memcpy(dst, src, p->w * 4);
    }
    HDC dc = GetDC(g_presentWnd); RECT rc; GetClientRect(g_presentWnd, &rc);
    int ox, oy, dw, dh; GetDest(g_presentWnd, p->w, p->h, ox, oy, dw, dh);
    static int lw, lh; if (lw != rc.right || lh != rc.bottom) { lw = rc.right; lh = rc.bottom; FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH)); }
    BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = p->w; bi.bmiHeader.biHeight = -p->h; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, ox, oy, dw, dh, 0, 0, p->w, p->h, g_bgra, &bi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(g_presentWnd, dc);
}
static DWORD WINAPI GdiThread(LPVOID) {
    // The game keeps the primary surface locked and writes into it directly (no Unlock per frame),
    // so dirty tracking is unreliable: present continuously at ~60 Hz.
    for (;;) { Sleep(16); g_dirty = 0; Present(); }
}
static void MarkDirty(Surf* s) { if (s == g_primary) InterlockedExchange(&g_dirty, 1); }

// ---------------------------------------------------------------- mouse mapping (window is larger than the 1024x768 game space)
static WNDPROC g_origProc; static HWND g_subWnd;
static LRESULT CALLBACK SubProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m >= 0x200 && m <= 0x209 && m != 0x20A && g_primary) {
        int x = (short)LOWORD(l), y = (short)HIWORD(l), ox, oy, dw, dh; GetDest(h, g_primary->w, g_primary->h, ox, oy, dw, dh);
        x = (x - ox) * g_primary->w / (dw ? dw : 1); y = (y - oy) * g_primary->h / (dh ? dh : 1);
        if (x < 0) x = 0; if (y < 0) y = 0; if (x >= g_primary->w) x = g_primary->w - 1; if (y >= g_primary->h) y = g_primary->h - 1;
        l = MAKELPARAM(x, y);
    }
    return CallWindowProcA(g_origProc, h, m, w, l);
}
static void Subclass(HWND h) {
    if (!h || g_subWnd == h) return; g_subWnd = h; g_origProc = (WNDPROC)SetWindowLongA(h, GWL_WNDPROC, (LONG)SubProc); Log("window %p subclassed", h);
}
static BOOL (WINAPI* g_SetCursorPos)(int, int);
static BOOL WINAPI H_SetCursorPos(int x, int y) {
    if (g_primary && g_subWnd) { int ox, oy, dw, dh; GetDest(g_subWnd, g_primary->w, g_primary->h, ox, oy, dw, dh); POINT pt = { ox + x * dw / g_primary->w, oy + y * dh / g_primary->h }; ClientToScreen(g_subWnd, &pt); x = pt.x; y = pt.y; }
    return g_SetCursorPos(x, y);
}

static BOOL (WINAPI* g_ClipCursor)(const RECT*);
static BOOL WINAPI H_ClipCursor(const RECT* r) {
    // The game clips the cursor to its 1024x768 space in screen coordinates; remap to the presented area.
    if (r && g_subWnd) {
        RECT cr; GetClientRect(g_subWnd, &cr); int ox = 0, oy = 0, dw = cr.right, dh = cr.bottom;
        if (g_primary) GetDest(g_subWnd, g_primary->w, g_primary->h, ox, oy, dw, dh);
        POINT a = { ox, oy }, b = { ox + dw, oy + dh }; ClientToScreen(g_subWnd, &a); ClientToScreen(g_subWnd, &b);
        RECT nr = { a.x, a.y, b.x, b.y }; Log("ClipCursor game(%d,%d,%d,%d) -> screen(%d,%d,%d,%d)", r->left, r->top, r->right, r->bottom, nr.left, nr.top, nr.right, nr.bottom);
        return g_ClipCursor(&nr);
    }
    return g_ClipCursor(r);
}

#include "patches.inc"
#include "present_d3d11.inc"

// ---------------------------------------------------------------- IUnknown
struct GK { GUID g; Kind k; };
static const GK kGuids[] = {
    {{0x6c14db80,0xa7b9,0x11ce,{0xa2,0xde,0x00,0xaa,0x00,0xb9,0x33,0x56}}, DD1},
    {{0xb3a6f3e0,0x2b43,0x11cf,{0xa2,0xde,0x00,0xaa,0x00,0xb9,0x33,0x56}}, DD2},
    {{0x9c59509a,0x39bd,0x11d1,{0x8c,0x4a,0x00,0xc0,0x4f,0xd9,0x30,0xc5}}, DD4},
    {{0x15e65ec0,0x3b9c,0x11d2,{0xb9,0x2f,0x00,0x60,0x97,0x97,0xea,0x5b}}, DD7},
    {{0x6c14db81,0xa7b9,0x11ce,{0xa2,0xde,0x00,0xaa,0x00,0xb9,0x33,0x56}}, S1},
    {{0x57805885,0x6eec,0x11cf,{0x94,0x41,0xa8,0x23,0x03,0xc1,0x0e,0x27}}, S2},
    {{0xda044e00,0x69b2,0x11d0,{0xa1,0xd5,0x00,0xaa,0x00,0xb8,0xdf,0xbb}}, S3},
    {{0x0b2b8630,0xad35,0x11d0,{0x8e,0xa6,0x00,0x60,0x97,0x97,0xea,0x5b}}, S4},
    {{0x06675a80,0x3b9b,0x11d2,{0xb9,0x2f,0x00,0x60,0x97,0x97,0xea,0x5b}}, S7},
    {{0x6c14db84,0xa733,0x11ce,{0xa5,0x21,0x00,0x20,0xaf,0x0b,0xe5,0x60}}, PAL},
    {{0x6c14db85,0xa735,0x11ce,{0xa5,0x21,0x00,0x20,0xaf,0x0b,0xe5,0x60}}, CLIP},
};
static HRESULT __stdcall H_QI(void* self, REFIID riid, void** out) {
    Obj* o = (Obj*)self; if (!out) return DDERR_INVALIDPARAMS;
    if (IsEqualGUID(riid, IID_IUnknown)) { *out = self; InterlockedIncrement(&o->impl->refs); return DD_OK; }
    for (auto& gk : kGuids) if (IsEqualGUID(gk.g, riid)) {
        bool ok = (IsDD(gk.k) && o->impl->type == T_DD) || (IsSurf(gk.k) && o->impl->type == T_SURF) || (gk.k == PAL && o->impl->type == T_PAL) || (gk.k == CLIP && o->impl->type == T_CLIP);
        if (!ok) break;
        *out = Face(o->impl, gk.k); InterlockedIncrement(&o->impl->refs);
        TRACE(o->kind, 0, "-> %s", kName[gk.k]); return DD_OK;
    }
    char gs[64]; _snprintf(gs, sizeof gs, "{%08lX-%04X-%04X-...}", riid.Data1, riid.Data2, riid.Data3);
    Log("%s::QueryInterface UNSUPPORTED %s", kName[o->kind], gs);
    *out = nullptr; return (HRESULT)0x80004002;
}
static ULONG __stdcall H_AddRef(void* self) { return InterlockedIncrement(&I(self)->refs); }
static ULONG __stdcall H_Release(void* self) {
    Impl* im = I(self); LONG r = InterlockedDecrement(&im->refs);
    if (r <= 0 && im->type == T_SURF) { Surf* s = (Surf*)im; Log("surface %p released (%dx%d bpp%d caps=0x%X)", s, s->w, s->h, s->bpp, s->caps); }
    return r < 0 ? 0 : r;
}

// ---------------------------------------------------------------- IDirectDraw
static HRESULT __stdcall DD_Ok1(void* self) { return DD_OK; }
static HRESULT __stdcall DD_Ok2(void* self, void*) { return DD_OK; }
static HRESULT __stdcall DD_Ok3(void* self, void*, void*) { return DD_OK; }
static HRESULT __stdcall DD_SetCoop(void* self, HWND hwnd, DWORD flags) {
    DD* d = D(self); d->hwnd = hwnd; d->coop = flags; if (hwnd) { g_presentWnd = hwnd; Subclass(hwnd); g_mainTid = GetWindowThreadProcessId(hwnd, nullptr); }
    TRACE(K(self), 20, "hwnd=%p flags=0x%X", hwnd, flags); return DD_OK;
}
static void SpritesReset();                                   // sprites.inc: the game reloads its sprite banks around a mode change -> forget the atlas
static HRESULT __stdcall DD_SetDisplayMode1(void* self, DWORD w, DWORD h, DWORD bpp) {
    SpritesReset(); DD* d = D(self); d->w = w; d->h = h; d->bpp = bpp; TRACE(K(self), 21, "%ux%u bpp=%u", w, h, bpp); return DD_OK;
}
static HRESULT __stdcall DD_SetDisplayMode2(void* self, DWORD w, DWORD h, DWORD bpp, DWORD hz, DWORD fl) {
    SpritesReset(); DD* d = D(self); d->w = w; d->h = h; d->bpp = bpp; TRACE(K(self), 21, "%ux%u bpp=%u hz=%u flags=0x%X", w, h, bpp, hz, fl); return DD_OK;
}
static HRESULT __stdcall DD_GetDisplayMode(void* self, void* desc) {
    DD* d = D(self); BYTE* b = (BYTE*)desc; DWORD sz = *(DWORD*)b; memset(b, 0, sz); *(DWORD*)b = sz;
    DWORD* p = (DWORD*)b; p[1] = 0x2 | 0x4 | 0x8 | 0x1000 | 0x40000; p[2] = d->h; p[3] = d->w; p[4] = d->w * ((d->bpp + 7) / 8); p[6] = 60;
    PixFmt pf; FillPF(pf, d->bpp); PutPF(b + 72, pf);
    TRACE(K(self), 12, "-> %dx%d bpp%d", d->w, d->h, d->bpp); return DD_OK;
}
static HRESULT __stdcall DD_EnumModes(void* self, DWORD flags, void* desc, void* ctx, void* cb) {
    Kind k = K(self); DWORD sz = (k == DD4 || k == DD7) ? 124 : 108;
    static std::vector<std::pair<int, int>> res;
    if (res.empty()) {
        res = {{640,480},{800,600},{1024,768},{1280,720},{1280,1024},{1600,900},{1920,1080}};
        char ex[256]; GetPrivateProfileStringA("modes", "extra", "", ex, sizeof ex, ".\\remaster.ini");     // optional extra modes, e.g. extra=2560x1440,2048x1152
        for (char* t = strtok(ex, ", ;"); t; t = strtok(nullptr, ", ;")) { int w = 0, h = 0; if (sscanf(t, "%dx%d", &w, &h) == 2 && w >= 640 && h >= 480 && w <= 7680 && h <= 4320) { res.push_back({w, h}); Log("extra display mode offered: %dx%d (experimental)", w, h); } }
    }
    static const int bpps[] = {8, 16, 32}; int n = 0;
    for (auto& rp : res) for (int bpp : bpps) { int r[2] = {rp.first, rp.second};
        BYTE buf[128] = {}; DWORD* p = (DWORD*)buf; p[0] = sz; p[1] = 0x2 | 0x4 | 0x8 | 0x1000 | 0x40000;
        p[2] = r[1]; p[3] = r[0]; p[4] = r[0] * ((bpp + 7) / 8); p[6] = 60; PixFmt pf; FillPF(pf, bpp); PutPF(buf + 72, pf);
        n++;
        if (((HRESULT(__stdcall*)(void*, void*))cb)(buf, ctx) == 0) break;
    }
    TRACE(k, 8, "flags=0x%X desc=%p -> %d modes offered", flags, desc, n); return DD_OK;
}
static HRESULT __stdcall DD_GetCaps(void* self, void* hal, void* hel) {
    if (hal) { DWORD sz = *(DWORD*)hal; memset(hal, 0, sz); *(DWORD*)hal = sz; DWORD* p = (DWORD*)hal; p[1] = 0x1 | 0x40 | 0x400000 | 0x800000; p[13] = 64 << 20; p[14] = 64 << 20; } // caps + vid mem
    if (hel) { DWORD sz = *(DWORD*)hel; memset(hel, 0, sz); *(DWORD*)hel = sz; }
    TRACE(K(self), 11, "hal=%p hel=%p", hal, hel); return DD_OK;
}
static HRESULT __stdcall DD_GetAvail(void* self, void* caps, DWORD* total, DWORD* fr) { if (total) *total = 64 << 20; if (fr) *fr = 48 << 20; return DD_OK; }
static HRESULT __stdcall DD_GetFourCC(void* self, DWORD* n, DWORD* codes) { if (n) *n = 0; return DD_OK; }
static HRESULT __stdcall DD_GetMonFreq(void* self, DWORD* f) { if (f) *f = 60; return DD_OK; }
static HRESULT __stdcall DD_GetScan(void* self, DWORD* l) { if (l) *l = 0; return DD_OK; }
static HRESULT __stdcall DD_GetVBlank(void* self, BOOL* b) { if (b) *b = TRUE; return DD_OK; }
static HRESULT __stdcall DD_WaitVBlank(void* self, DWORD fl, HANDLE ev) { Sleep(1); return DD_OK; }
static HRESULT __stdcall DD_GetGDISurface(void* self, void** out) {
    DD* d = D(self); if (!d->primary) return DDERR_NOTFOUND; *out = Face(d->primary, K(self) == DD7 ? S7 : K(self) == DD4 ? S4 : S1); d->primary->refs++; return DD_OK;
}
static HRESULT __stdcall DD_CreatePalette(void* self, DWORD flags, PALETTEENTRY* e, void** out, void* outer) {
    Pal* p = new Pal(); memset((Impl*)p, 0, sizeof(Impl)); memset(p->e, 0, sizeof p->e); p->type = T_PAL;
    if (e) memcpy(p->e, e, 256 * 4); // DDPCAPS_8BIT palettes: 256 entries
    p->allow256 = (flags & 0x40) != 0; PalReserve(p);
    p->refs = 1; *out = Face(p, PAL); g_lastPal = p; TRACE(K(self), 5, "flags=0x%X", flags); return DD_OK;
}
static HRESULT __stdcall DD_CreateClipper(void* self, DWORD flags, void** out, void* outer) {
    Clip* c = new Clip(); memset((Impl*)c, 0, sizeof(Impl)); c->type = T_CLIP; c->refs = 1; c->hwnd = nullptr; *out = Face(c, CLIP); return DD_OK;
}
static Kind SurfKindFor(Kind dk) { return dk == DD7 ? S7 : dk == DD4 ? S4 : S1; }
static HRESULT __stdcall DD_CreateSurface(void* self, void* desc, void** out, void* outer) {
    DD* d = D(self); Kind sk = SurfKindFor(K(self)); BYTE* b = (BYTE*)desc; DWORD* p = (DWORD*)b;
    DWORD fl = p[1], caps = *(DWORD*)(b + 104); int w = p[3], h = p[2];
    char dump[400]; int o = 0; for (int i = 0; i < 31 && i < (int)(p[0] / 4); i++) o += sprintf(dump + o, "%X ", p[i]);
    Log("%s::CreateSurface desc[%u]: %s", kName[K(self)], p[0], dump);
    if (!out) return DDERR_INVALIDPARAMS;
    if (caps & 0x200) { // primary
        Surf* s = NewSurf(d, d->w, d->h, d->bpp, nullptr, caps | 0x200); s->primary = true; s->refs = 1; d->primary = s; g_primary = s; g_dirty = 1;
        if ((fl & 0x20) && p[5] > 0) { // back buffer(s)
            Surf* bk = NewSurf(d, d->w, d->h, d->bpp, nullptr, 0x4 | 0x8 | 0x800); bk->front = s; s->back = bk; bk->refs = 1; // refs held by primary
            s->caps |= 0x8 | 0x10;
        }
        *out = Face(s, sk); Log("  -> primary %dx%d bpp%d back=%p", s->w, s->h, s->bpp, s->back); return DD_OK;
    }
    PixFmt pf; const PixFmt* ppf = nullptr; int bpp = d->bpp;
    if (fl & 0x1000) { DWORD* q = (DWORD*)(b + 72); pf = {q[1], q[3], q[4], q[5], q[6], q[7]}; ppf = &pf; bpp = pf.rgbBits; }
    Surf* s = NewSurf(d, w, h, bpp, ppf, caps); s->refs = 1; *out = Face(s, sk);
    Log("  -> offscreen %p %dx%d bpp%d caps=0x%X", s, w, h, bpp, caps); return DD_OK;
}
static HRESULT __stdcall DD_RestoreAll(void* self) { return DD_OK; }

// ---------------------------------------------------------------- IDirectDrawPalette / Clipper
static HRESULT __stdcall PAL_GetCaps(void* self, DWORD* c) { if (c) *c = 0x4; return DD_OK; }
static HRESULT __stdcall PAL_GetEntries(void* self, DWORD fl, DWORD start, DWORD n, PALETTEENTRY* e) {
    Pal* p = (Pal*)I(self); if (start + n > 256) return DDERR_INVALIDPARAMS; memcpy(e, p->e + start, n * 4); return DD_OK;
}
static HRESULT __stdcall PAL_SetEntries(void* self, DWORD fl, DWORD start, DWORD n, PALETTEENTRY* e) {
    Pal* p = (Pal*)I(self); if (start + n > 256) return DDERR_INVALIDPARAMS; memcpy(p->e + start, e, n * 4); PalReserve(p);
    g_dirty = 1; return DD_OK;
}
static HRESULT __stdcall CLIP_SetHWnd(void* self, DWORD fl, HWND h) { ((Clip*)I(self))->hwnd = h; return DD_OK; }
static HRESULT __stdcall CLIP_Changed(void* self, BOOL* b) { if (b) *b = FALSE; return DD_OK; }
static HRESULT __stdcall CLIP_SetList(void* self, void* l, DWORD f) { return DD_OK; }

// ---------------------------------------------------------------- surfaces
static HRESULT __stdcall SF_Lock(void* self, RECT* r, void* desc, DWORD flags, HANDLE ev) {
    Surf* s = S(self); FillDesc(s, (BYTE*)desc, true);
    if (r) *(DWORD*)((BYTE*)desc + 36) = (DWORD)(s->mem + r->top * s->pitch + r->left * ((s->bpp + 7) / 8));
    TRACE(K(self), 25, "%s rect=%s flags=0x%X", s->primary ? "primary" : "surf", r ? "yes" : "no", flags); return DD_OK;
}
static HRESULT __stdcall SF_Unlock(void* self, void* p) { MarkDirty(S(self)); return DD_OK; }
static HRESULT __stdcall SF_GetDesc(void* self, void* desc) { FillDesc(S(self), (BYTE*)desc, false); return DD_OK; }
static HRESULT __stdcall SF_GetPF(void* self, void* pf) { PutPF((BYTE*)pf, S(self)->pf); return DD_OK; }
static HRESULT __stdcall SF_GetCaps(void* self, DWORD* caps) { if (caps) { memset(caps, 0, 4); caps[0] = S(self)->caps; } return DD_OK; }
static HRESULT __stdcall SF_GetAttached(void* self, DWORD* caps, void** out) {
    Surf* s = S(self); DWORD c = caps ? caps[0] : 0; TRACE(K(self), 12, "caps=0x%X", c);
    if ((c & 0x4) && s->back) { s->back->refs++; *out = Face(s->back, K(self)); return DD_OK; }
    *out = nullptr; return DDERR_NOTFOUND;
}
static void Copy(Surf* dst, const RECT* dr, Surf* src, const RECT* sr, bool key, bool fill, DWORD color) {
    RECT d = dr ? *dr : RECT{0, 0, dst->w, dst->h}; RECT r = sr ? *sr : RECT{0, 0, src ? src->w : 0, src ? src->h : 0};
    if (d.left < 0) { if (!fill) r.left -= d.left; d.left = 0; } if (d.top < 0) { if (!fill) r.top -= d.top; d.top = 0; }
    if (d.right > dst->w) d.right = dst->w; if (d.bottom > dst->h) d.bottom = dst->h;
    int dw = d.right - d.left, dh = d.bottom - d.top; if (dw <= 0 || dh <= 0) return;
    int bp = (dst->bpp + 7) / 8;
    if (fill) {
        for (int y = 0; y < dh; y++) { u8* row = dst->mem + (d.top + y) * dst->pitch + d.left * bp;
            if (bp == 1) memset(row, (u8)color, dw); else if (bp == 2) for (int x = 0; x < dw; x++) ((WORD*)row)[x] = (WORD)color; else for (int x = 0; x < dw; x++) ((DWORD*)row)[x] = color; }
        return;
    }
    if (!src || src->bpp != dst->bpp) { Log("Blt: format mismatch dst=%d src=%d", dst->bpp, src ? src->bpp : -1); return; }
    int sw = r.right - r.left, sh = r.bottom - r.top; if (sw <= 0 || sh <= 0) return;
    if (r.left < 0 || r.top < 0 || r.right > src->w || r.bottom > src->h) { // clamp source
        if (r.left < 0) r.left = 0; if (r.top < 0) r.top = 0; if (r.right > src->w) r.right = src->w; if (r.bottom > src->h) r.bottom = src->h; sw = r.right - r.left; sh = r.bottom - r.top; if (sw <= 0 || sh <= 0) return;
    }
    bool ck = key && src->hasCk;
    for (int y = 0; y < dh; y++) {
        int sy = r.top + (sw == dw && sh == dh ? y : y * sh / dh);
        const u8* srow = src->mem + sy * src->pitch + r.left * bp; u8* drow = dst->mem + (d.top + y) * dst->pitch + d.left * bp;
        if (!ck && sw == dw) { memmove(drow, srow, dw * bp); continue; }
        for (int x = 0; x < dw; x++) {
            int sx = (sw == dw) ? x : x * sw / dw; DWORD v = bp == 1 ? srow[sx] : bp == 2 ? ((const WORD*)srow)[sx] : ((const DWORD*)srow)[sx];
            if (ck && v >= src->ckLow && v <= src->ckHigh) continue;
            if (bp == 1) drow[x] = (u8)v; else if (bp == 2) ((WORD*)drow)[x] = (WORD)v; else ((DWORD*)drow)[x] = v;
        }
    }
}
static HRESULT __stdcall SF_Blt(void* self, RECT* dr, void* srcFace, RECT* sr, DWORD flags, void* fx) {
    Surf* d = S(self); Surf* s = srcFace ? S(srcFace) : nullptr;
    TRACE(K(self), 5, "%s <- %s flags=0x%X", d->primary ? "primary" : "surf", s ? (s->primary ? "primary" : (s->front ? "back" : "surf")) : "none", flags);
    if (flags & 0x400) Copy(d, dr, nullptr, nullptr, false, true, fx ? ((DWORD*)fx)[20] : 0);
    else Copy(d, dr, s, sr, (flags & 0x8000) != 0, false, 0);
    MarkDirty(d); return DD_OK;
}
static HRESULT __stdcall SF_BltFast(void* self, DWORD x, DWORD y, void* srcFace, RECT* sr, DWORD trans) {
    Surf* d = S(self); Surf* s = S(srcFace); RECT r = sr ? *sr : RECT{0, 0, s->w, s->h}; RECT dr = {(LONG)x, (LONG)y, (LONG)x + (r.right - r.left), (LONG)y + (r.bottom - r.top)};
    TRACE(K(self), 7, "(%u,%u) trans=0x%X", x, y, trans);
    Copy(d, &dr, s, &r, (trans & 1) != 0, false, 0); MarkDirty(d); return DD_OK;
}
static HRESULT __stdcall SF_Flip(void* self, void* target, DWORD flags) {
    Surf* s = S(self); TRACE(K(self), 11, "target=%p flags=0x%X", target, flags);
    if (s->back) { memcpy(s->mem, s->back->mem, (size_t)s->pitch * s->h); }
    MarkDirty(s); return DD_OK;
}
static HRESULT __stdcall SF_SetPalette(void* self, void* pal) {
    Surf* s = S(self); s->pal = pal ? (Pal*)I(pal) : nullptr; if (s->pal) g_lastPal = s->pal; TRACE(K(self), 31, "pal=%p", pal); g_dirty = 1; return DD_OK;
}
static HRESULT __stdcall SF_GetPalette(void* self, void** out) { Surf* s = S(self); if (!s->pal) { *out = nullptr; return DDERR_NOTFOUND; } s->pal->refs++; *out = Face(s->pal, PAL); return DD_OK; }
static HRESULT __stdcall SF_SetClipper(void* self, void* c) { S(self)->clip = c ? (Clip*)I(c) : nullptr; return DD_OK; }
static HRESULT __stdcall SF_GetClipper(void* self, void** out) { Surf* s = S(self); if (!s->clip) { *out = nullptr; return DDERR_NOTFOUND; } s->clip->refs++; *out = Face(s->clip, CLIP); return DD_OK; }
static HRESULT __stdcall SF_SetColorKey(void* self, DWORD fl, DWORD* ck) {
    Surf* s = S(self); if (ck) { s->ckLow = ck[0]; s->ckHigh = ck[1]; s->hasCk = true; } TRACE(K(self), 29, "flags=0x%X low=0x%X high=0x%X", fl, ck ? ck[0] : 0, ck ? ck[1] : 0); return DD_OK;
}
static HRESULT __stdcall SF_GetColorKey(void* self, DWORD fl, DWORD* ck) { Surf* s = S(self); if (!s->hasCk) return (HRESULT)0x8876007E; ck[0] = s->ckLow; ck[1] = s->ckHigh; return DD_OK; }
static HRESULT __stdcall SF_IsLost(void* self) { return DD_OK; }
static HRESULT __stdcall SF_Restore(void* self) { return DD_OK; }
static HRESULT __stdcall SF_Status(void* self, DWORD fl) { return DD_OK; }
static HRESULT __stdcall SF_AddAtt(void* self, void* o) { return DD_OK; }
static HRESULT __stdcall SF_DelAtt(void* self, DWORD f, void* o) { return DD_OK; }
static HRESULT __stdcall SF_PageLock(void* self, DWORD f) { return DD_OK; }

// ---------------------------------------------------------------- vtable generation
static BYTE* g_stubMem; static int g_stubUsed; struct StubInfo { Kind k; int i; }; static StubInfo g_stubs[1024]; static int g_nStubs;
extern "C" void __cdecl StubLog(int id, DWORD* a) {
    StubInfo& si = g_stubs[id]; unsigned long n = ++g_cnt[si.k][si.i];
    if (n <= 12 || !(n & (n - 1))) Log("UNIMPLEMENTED %s::%s #%lu this=%p args: %X %X %X %X %X", kName[si.k], NameOf(si.k, si.i), n, (void*)a[0], a[1], a[2], a[3], a[4], a[5]);
}
static void* MakeStub(Kind k, int i) {
    int id = g_nStubs++; g_stubs[id] = {k, i};
    BYTE* p = g_stubMem + g_stubUsed; BYTE* q = p; g_stubUsed += 48;
    *q++ = 0x60; *q++ = 0x8D; *q++ = 0x44; *q++ = 0x24; *q++ = 0x24; *q++ = 0x50;
    *q++ = 0x68; *(int*)q = id; q += 4; *q++ = 0xB9; *(void**)q = (void*)&StubLog; q += 4; *q++ = 0xFF; *q++ = 0xD1;
    *q++ = 0x83; *q++ = 0xC4; *q++ = 0x08; *q++ = 0x61;
    *q++ = 0xB8; *(DWORD*)q = (DWORD)E_NOTIMPL_; q += 4;                      // mov eax, E_NOTIMPL
    int bytes = (ArgsOf(k, i)) * 4; if (bytes) { *q++ = 0xC2; *(WORD*)q = (WORD)bytes; q += 2; } else *q++ = 0xC3;
    return p;
}
static void BuildTables() {
    g_stubMem = (BYTE*)VirtualAlloc(nullptr, 1 << 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    // handlers
    for (int k = 0; k < KINDS; k++) { g_h[k][0] = (void*)H_QI; g_h[k][1] = (void*)H_AddRef; g_h[k][2] = (void*)H_Release; }
    for (int k = DD1; k <= DD7; k++) {
        g_h[k][3] = (void*)DD_Ok1; g_h[k][4] = (void*)DD_CreateClipper; g_h[k][5] = (void*)DD_CreatePalette; g_h[k][6] = (void*)DD_CreateSurface;
        g_h[k][8] = (void*)DD_EnumModes; g_h[k][10] = (void*)DD_Ok1; g_h[k][11] = (void*)DD_GetCaps; g_h[k][12] = (void*)DD_GetDisplayMode;
        g_h[k][13] = (void*)DD_GetFourCC; g_h[k][14] = (void*)DD_GetGDISurface; g_h[k][15] = (void*)DD_GetMonFreq; g_h[k][16] = (void*)DD_GetScan;
        g_h[k][17] = (void*)DD_GetVBlank; g_h[k][18] = (void*)DD_Ok2; g_h[k][19] = (void*)DD_Ok1; g_h[k][20] = (void*)DD_SetCoop;
        g_h[k][21] = (void*)(k == DD1 ? (void*)DD_SetDisplayMode1 : (void*)DD_SetDisplayMode2); g_h[k][22] = (void*)DD_WaitVBlank;
    }
    for (int k = DD2; k <= DD7; k++) g_h[k][23] = (void*)DD_GetAvail;
    for (int k = DD4; k <= DD7; k++) { g_h[k][25] = (void*)DD_RestoreAll; g_h[k][26] = (void*)DD_Ok1; }
    for (int k = S1; k <= S7; k++) {
        g_h[k][5] = (void*)SF_Blt; g_h[k][7] = (void*)SF_BltFast; g_h[k][3] = (void*)SF_AddAtt; g_h[k][8] = (void*)SF_DelAtt; g_h[k][11] = (void*)SF_Flip;
        g_h[k][12] = (void*)SF_GetAttached; g_h[k][13] = (void*)SF_Status; g_h[k][14] = (void*)SF_GetCaps; g_h[k][15] = (void*)SF_GetClipper; g_h[k][16] = (void*)SF_GetColorKey;
        g_h[k][18] = (void*)SF_Status; g_h[k][20] = (void*)SF_GetPalette; g_h[k][21] = (void*)SF_GetPF; g_h[k][22] = (void*)SF_GetDesc; g_h[k][24] = (void*)SF_IsLost;
        g_h[k][25] = (void*)SF_Lock; g_h[k][27] = (void*)SF_Restore; g_h[k][28] = (void*)SF_SetClipper; g_h[k][29] = (void*)SF_SetColorKey; g_h[k][31] = (void*)SF_SetPalette; g_h[k][32] = (void*)SF_Unlock;
    }
    for (int k = S2; k <= S7; k++) { g_h[k][37] = (void*)SF_PageLock; g_h[k][38] = (void*)SF_PageLock; }
    g_h[PAL][3] = (void*)PAL_GetCaps; g_h[PAL][4] = (void*)PAL_GetEntries; g_h[PAL][6] = (void*)PAL_SetEntries;
    g_h[CLIP][4] = (void*)DD_Ok2; g_h[CLIP][6] = (void*)CLIP_Changed; g_h[CLIP][7] = (void*)CLIP_SetList; g_h[CLIP][8] = (void*)CLIP_SetHWnd;
    // fix: Lock/Unlock/others implemented above use correct arities; unimplemented methods get logging stubs
    for (int k = 0; k < KINDS; k++) for (int i = 0; i < kCount[k]; i++) g_vt[k][i] = g_h[k][i] ? g_h[k][i] : MakeStub((Kind)k, i);
}

#include "profiler.inc"

// ---------------------------------------------------------------- exports
static DD* NewDD() { DD* d = new DD(); d->type = T_DD; d->refs = 0; memset(d->faces, 0, sizeof d->faces); return d; }
extern "C" __declspec(dllexport) HRESULT WINAPI DirectDrawCreate(GUID* g, void** out, IUnknown* outer) {
    Log("DirectDrawCreate guid=%p", g); DD* d = NewDD(); d->refs = 1; *out = Face(d, DD1); return DD_OK;
}
extern "C" __declspec(dllexport) HRESULT WINAPI DirectDrawCreateEx(GUID* g, void** out, REFIID riid, IUnknown* outer) {
    Log("DirectDrawCreateEx"); DD* d = NewDD(); d->refs = 1; *out = Face(d, DD7); return DD_OK;
}
extern "C" __declspec(dllexport) HRESULT WINAPI DirectDrawEnumerateA(LPVOID cb, LPVOID ctx) {
    typedef BOOL(WINAPI* CB)(GUID*, LPSTR, LPSTR, LPVOID); ((CB)cb)(nullptr, (LPSTR)"Primary Display Driver", (LPSTR)"display", ctx); return DD_OK;
}
extern "C" __declspec(dllexport) HRESULT WINAPI DirectDrawCreateClipper(DWORD f, void** out, IUnknown* outer) { Clip* c = new Clip(); memset((Impl*)c, 0, sizeof(Impl)); c->type = T_CLIP; c->refs = 1; *out = Face(c, CLIP); return DD_OK; }

// COM creation path (CoCreateInstance for CLSID_DirectDraw / CLSID_DirectDraw7)
static const GUID CLSID_DD  = {0xD7B70EE0,0x4340,0x11CF,{0xB0,0x63,0x00,0x20,0xAF,0xC2,0xCD,0x35}};
static const GUID CLSID_DD7 = {0x3C305196,0x50DB,0x11D3,{0x9C,0xFE,0x00,0xC0,0x4F,0xD9,0x30,0xC5}};
typedef HRESULT(WINAPI* PFN_CoCreate)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
static PFN_CoCreate g_realCoCreate;
static HRESULT WINAPI H_CoCreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD ctx, REFIID riid, LPVOID* out) {
    if (IsEqualGUID(clsid, CLSID_DD) || IsEqualGUID(clsid, CLSID_DD7)) {
        Log("CoCreateInstance(DirectDraw)"); DD* d = NewDD(); d->refs = 1; Obj* f = Face(d, DD1); return H_QI(f, riid, out);
    }
    return g_realCoCreate(clsid, outer, ctx, riid, out);
}
static void* PatchImp(const char* name, void* repl) {
    BYTE* base = (BYTE*)GetModuleHandleA(nullptr);
    auto dos = (PIMAGE_DOS_HEADER)base; auto nt = (PIMAGE_NT_HEADERS)(base + dos->e_lfanew);
    auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]; if (!dir.VirtualAddress) return nullptr;
    for (auto d = (PIMAGE_IMPORT_DESCRIPTOR)(base + dir.VirtualAddress); d->Name; d++) {
        auto thunk = (PIMAGE_THUNK_DATA)(base + d->FirstThunk); auto orig = (PIMAGE_THUNK_DATA)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        for (; orig->u1.AddressOfData; orig++, thunk++) {
            if (orig->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            if (strcmp((char*)((PIMAGE_IMPORT_BY_NAME)(base + orig->u1.AddressOfData))->Name, name) == 0) {
                DWORD old; VirtualProtect(&thunk->u1.Function, 4, PAGE_READWRITE, &old);
                void* o = (void*)thunk->u1.Function; thunk->u1.Function = (DWORD)repl; VirtualProtect(&thunk->u1.Function, 4, old, &old); return o;
            }
        }
    }
    return nullptr;
}
static void PatchCoCreate() {
    g_realCoCreate = (PFN_CoCreate)PatchImp("CoCreateInstance", (void*)H_CoCreateInstance);
    g_SetCursorPos = (decltype(g_SetCursorPos))PatchImp("SetCursorPos", (void*)H_SetCursorPos);
    g_ClipCursor = (decltype(g_ClipCursor))PatchImp("ClipCursor", (void*)H_ClipCursor);
}
static LONG CALLBACK VEH(PEXCEPTION_POINTERS ep) {
    DWORD c = ep->ExceptionRecord->ExceptionCode;
    if (c == 0xC0000005 || c == 0xC0000094 || c == 0xC000001D) {
        Log("EXCEPTION %08X at %p tid=%lu", c, ep->ExceptionRecord->ExceptionAddress, GetCurrentThreadId());
        const CONTEXT* x = ep->ContextRecord;
        Log("  eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X ebp=%08X esp=%08X; access %s %p", x->Eax, x->Ebx, x->Ecx, x->Edx, x->Esi, x->Edi, x->Ebp, x->Esp,
            ep->ExceptionRecord->NumberParameters >= 2 ? (ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read") : "?", ep->ExceptionRecord->NumberParameters >= 2 ? (void*)ep->ExceptionRecord->ExceptionInformation[1] : nullptr);
        char line[600]; int n = 0, found = 0; DWORD ddr = (DWORD)GetModuleHandleA("ddraw.dll");
        __try { const DWORD* sp = (const DWORD*)x->Esp; for (int i = 0; i < 1024 && found < 24; i++) { DWORD v = sp[i];
            bool game = v >= 0x401000 && v < 0x5A0000, mine = ddr && v >= ddr && v < ddr + 0x200000;
            if (game || mine) { n += _snprintf(line + n, sizeof(line) - n - 1, " %s%X@%d", mine ? "ddraw+" : "", mine ? v - ddr : v, i); found++; if (n > 540) break; } } } __except (EXCEPTION_EXECUTE_HANDLER) {}
        line[n] = 0; Log("  stack hits:%s", line); if (g_log) fflush(g_log);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID rsv) {
    if (reason == DLL_PROCESS_ATTACH) {
        InitializeCriticalSection(&g_cs);
#ifdef REMASTER_DEV
        MoveFileExA("ddraw_d3d11.log", "ddraw_d3d11.prev.log", MOVEFILE_REPLACE_EXISTING); g_log = fopen("ddraw_d3d11.log", "w"); Log("own ddraw attached");
        AddVectoredExceptionHandler(1, VEH);
#endif
        timeBeginPeriod(1); BuildTables(); ApplyPatches(); PatchCoCreate();
        CreateThread(nullptr, 0, PresentThread, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        // Process exit (rsv != null): Windows has already killed our other threads - one of them may have been inside the heap or the log lock, and the
        // destructors of the DLL's globals (vectors, maps, shared_ptrs) would then wait for a lock nobody can release: a black-screen hang on quit.
        // Everything the game needs is already written, so end the process right here instead of running them.
        if (rsv) { if (g_log) fflush(g_log); TerminateProcess(GetCurrentProcess(), 0); }
    }
    return TRUE;
}
