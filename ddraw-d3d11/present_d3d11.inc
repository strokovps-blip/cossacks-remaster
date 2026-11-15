// D3D11 presenter (vsync at the monitor refresh rate). Included by ddraw.cpp after the object model.
static const char* kHlsl = R"(
cbuffer C : register(b0) { float4 src; float4 dest; float4 cur; float4 sm; float4 lay; };   // src: w,h,mode(0=indexed,1=rgba); dest: ox,oy,dw,dh (pixels); cur: x0,y0 (game px, top-left of cursor image), w,h (w<=0: no cursor)
Texture2D<float>  idxTex  : register(t0);
Texture2D<float4> palTex  : register(t1);
Texture2D<float4> rgbaTex : register(t2);
Texture2D<float>  curIdx  : register(t3);
Texture2D<float>  curMask : register(t4);
Texture2D<float>  worldTex : register(t6);   // pure world layer of the current frame (no interface)
Texture2D<float>  prevTex : register(t5);   // previous game frame (for scroll interpolation)
Texture2D<float4> rgbW : register(t10);     // true-colour world layer of the current frame (GPU world compositor)
Texture2D<float4> rgbP : register(t11);     // ... and of the previous one
cbuffer Q : register(b1) { float4 qa; float4 qb; float4 qc; };   // qa: scaler,sharp,gamma,contrast  qb: saturation,vignette,smooth,fxaa  qc: blend,0,0,0
Texture2D<float4> gameTex : register(t9);     // the finished game-resolution image (rgb, a = 1 for interface pixels)
SamplerState lin : register(s0);
float4 palIdx(float v) { uint i = (uint)(v * 255.0 + 0.5); return palTex.Load(int3(i,0,0)); }
// Interface areas that must not be shifted even though their pixels change every tick: the top resource bar and the minimap
// (fixed-size UI in the bottom-right corner: ~258x270 px at the screen edge). Everything else (unit panel, buttons, text)
// is recognised per pixel: it is unchanged between frames and does not match the shifted previous frame.
bool inHud(int2 p) {
    return p.y < int(src.y * 0.02) || (p.x >= int(src.x) - 262 && p.y >= int(src.y) - 274);
}
// sm: x,y = world shift (pixels) between the previous and the current game frame, z = interpolation fraction 0..1, w = enabled
float4 DW(int2 q) {                                                // true-colour world: a dithered (checkerboard) effect becomes an even translucency (2x2 average), so it no longer crawls when the picture is shifted
    int2 b = q & ~1; float4 a0 = rgbW.Load(int3(b,0)), a1 = rgbW.Load(int3(b + int2(1,0),0)), a2 = rgbW.Load(int3(b + int2(0,1),0)), a3 = rgbW.Load(int3(b + int2(1,1),0));
    if (max(max(a0.a, a1.a), max(a2.a, a3.a)) < 0.5) return rgbW.Load(int3(q,0));
    return float4((a0.rgb + a1.rgb + a2.rgb + a3.rgb) * 0.25, 1);
}
float4 DP(int2 q) {
    int2 b = q & ~1; float4 a0 = rgbP.Load(int3(b,0)), a1 = rgbP.Load(int3(b + int2(1,0),0)), a2 = rgbP.Load(int3(b + int2(0,1),0)), a3 = rgbP.Load(int3(b + int2(1,1),0));
    if (max(max(a0.a, a1.a), max(a2.a, a3.a)) < 0.5) return rgbP.Load(int3(q,0));
    return float4((a0.rgb + a1.rgb + a2.rgb + a3.rgb) * 0.25, 1);
}
float4 fetch(int2 p) {
    int2 sz = int2(src.xy);
    p = clamp(p, int2(0,0), sz - 1);
    if (src.z < 0.5) {
        float c = idxTex.Load(int3(p,0));
        if (lay.w > 0.5) {                                        // the GPU world carries true colour: world pixels use it, interface pixels keep the palette
            if (p.y < 30 || (p.x >= int(src.x) - 262 && p.y >= int(src.y) - 274)) return palIdx(c);
            float w = worldTex.Load(int3(p,0));
            if (c != w) return palIdx(c);
            if (!(lay.x > 0.5 && sm.w > 0.5)) return DW(p);
            int2 v = int2(sm.xy); int2 sh = int2(round(sm.z * float2(v))); int2 sp = p - sh, sc = p - sh + v;
            bool inP = all(sp >= int2(0,9)) && all(sp < sz), inC = all(sc >= int2(0,9)) && all(sc < sz);
            if (qc.x > 0.5 && inP && inC) {
                float4 a = DP(sp), b = DW(sc); float d = abs(dot(a.rgb - b.rgb, float3(0.30, 0.59, 0.11)));
                return (d < 0.18) ? lerp(a, b, smoothstep(0.0, 1.0, sm.z)) : ((sm.z < 0.5) ? a : b);
            }
            if (sm.z < 0.5) { if (inP) return DP(sp); if (inC) return DW(sc); }
            else            { if (inC) return DW(sc); if (inP) return DP(sp); }
            return DW(p);
        }
        if (lay.x > 0.5 && sm.w > 0.5) {                          // layered mode: exact interface mask = final != world snapshot
            // The ground blit starts at y=9: rows 0..8 of the offscreen buffer are never rewritten and still hold LAST frame's
            // top bar, so they are not world. Treat them as interface.
            // Interface areas that are ALWAYS interface (the pixel diff misses interface pixels that happen to equal the world pixel,
            // which let shifted world content (a fence...) show through): top bar rows and the fixed-size minimap in the bottom-right corner.
            if (p.y < 30 || (p.x >= int(src.x) - 262 && p.y >= int(src.y) - 274)) return palIdx(c);
            float w = worldTex.Load(int3(p,0));
            if (c != w) return palIdx(c);                          // interface pixel: never shifted
            int2 v = int2(sm.xy);
            int2 sh = int2(round(sm.z * float2(v)));
            int2 sp = p - sh, sc = p - sh + v;                     // same world point in the previous / current world layer
            bool inP = all(sp >= int2(0,9)) && all(sp < sz), inC = all(sc >= int2(0,9)) && all(sc < sz);   // rows < 9 are stale
            if (qc.x > 0.5 && inP && inC) {                         // gentle changes (fog reveal, water, light) fade smoothly; large ones switch at half time
                float4 a = palIdx(prevTex.Load(int3(sp,0))), b = palIdx(worldTex.Load(int3(sc,0)));
                float d = abs(dot(a.rgb - b.rgb, float3(0.30, 0.59, 0.11)));
                return (d < 0.18) ? lerp(a, b, smoothstep(0.0, 1.0, sm.z)) : ((sm.z < 0.5) ? a : b);
            }
            if (sm.z < 0.5) { if (inP) return palIdx(prevTex.Load(int3(sp,0))); if (inC) return palIdx(worldTex.Load(int3(sc,0))); }
            else            { if (inC) return palIdx(worldTex.Load(int3(sc,0))); if (inP) return palIdx(prevTex.Load(int3(sp,0))); }
            return palIdx(w);
        }
        if (sm.w > 0.5 && !inHud(p)) {
            int2 v = int2(sm.xy);
            float pv = prevTex.Load(int3(p,0));
            int2 pm = p - v; bool pmIn = all(pm >= 0) && all(pm < sz);
            float pmv = pmIn ? prevTex.Load(int3(pm,0)) : -1.0;
            bool world = (pmv == c) || (c != pv);             // static (HUD) pixels stay where they are
            if (world) {
                int2 sh = int2(round(sm.z * float2(v)));
                int2 sp = p - sh, sc = p - sh + v;            // same world point in the previous / the current frame
                bool inP = all(sp >= 0) && all(sp < sz) && !inHud(sp), inC = all(sc >= 0) && all(sc < sz) && !inHud(sc);
                if (sm.z < 0.5) { if (inP) return palIdx(prevTex.Load(int3(sp,0))); if (inC) return palIdx(idxTex.Load(int3(sc,0))); }
                else            { if (inC) return palIdx(idxTex.Load(int3(sc,0))); if (inP) return palIdx(prevTex.Load(int3(sp,0))); }
            }
        }
        return palIdx(c);
    }
    return rgbaTex.Load(int3(p,0));
}
float4 VS(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2); return float4(uv * float2(2,-2) + float2(-1,1), 0, 1);
}
float4 PS(float4 pos : SV_Position) : SV_Target {
    float2 q = (pos.xy - dest.xy) / dest.zw;
    if (q.x < 0 || q.y < 0 || q.x >= 1 || q.y >= 1) return float4(0,0,0,1);
    float2 s = q * src.xy - 0.5; int2 i0 = (int2)floor(s); float2 f = frac(s);
    float2 k = max(dest.zw / src.xy, 1.0);              // output pixels per source texel
    f = saturate((f - 0.5) * k + 0.5);                   // "sharp bilinear": crisp texels, smooth edges
    float4 a = lerp(fetch(i0), fetch(i0 + int2(1,0)), f.x);
    float4 b = lerp(fetch(i0 + int2(0,1)), fetch(i0 + int2(1,1)), f.x);
    float3 col = lerp(a, b, f.y).rgb;
    if (cur.z > 0) {                                     // software cursor drawn at the live mouse position
        float2 cl = q * src.xy - cur.xy;
        if (cl.x >= 0 && cl.y >= 0 && cl.x < cur.z && cl.y < cur.w) {
            int2 ci = (int2)floor(cl);
            if (curMask.Load(int3(ci, 0)) > 0.5) { uint i = (uint)(curIdx.Load(int3(ci, 0)) * 255.0 + 0.5); col = palTex.Load(int3(i,0,0)).rgb; }
        }
    }
    return float4(col, 1);
}

)"
R"(
// ---- quality pipeline -------------------------------------------------------------------------------------------------------
bool uiPixel(int2 p) {                                      // true where the pixel belongs to the interface (never filtered)
    if (src.z > 0.5 || lay.z < 0.5) return true;             // no world layer known: treat everything as interface
    if (p.y < 30 || (p.x >= int(src.x) - 262 && p.y >= int(src.y) - 274)) return true;
    float w = worldTex.Load(int3(p,0));
    return idxTex.Load(int3(p,0)) != w;
}
float4 PS1(float4 pos : SV_Position) : SV_Target {          // pass 1: game-resolution image (layers, units, crossfade) + interface mask in alpha
    int2 p = int2(pos.xy);
    float4 c = fetch(p);
    return float4(c.rgb, uiPixel(p) ? 1.0 : 0.0);
}
float luma(float3 c) { return dot(c, float3(0.30, 0.59, 0.11)); }
float4 G4(int2 p) { return gameTex.Load(int3(clamp(p, int2(0,0), int2(src.xy) - 1), 0)); }
float4 PSF(float4 pos : SV_Position) : SV_Target {          // pass 1b: edge-preserving smoothing + sub-pixel anti-aliasing of the WORLD only
    int2 p = int2(pos.xy);
    float4 c = G4(p);
    if (c.a > 0.5) return c;
    float3 acc = c.rgb; float wsum = 1.0; float lc = luma(c.rgb);
    float4 n4[4]; n4[0] = G4(p + int2(0,-1)); n4[1] = G4(p + int2(0,1)); n4[2] = G4(p + int2(-1,0)); n4[3] = G4(p + int2(1,0));
    float4 d4[4]; d4[0] = G4(p + int2(-1,-1)); d4[1] = G4(p + int2(1,-1)); d4[2] = G4(p + int2(-1,1)); d4[3] = G4(p + int2(1,1));
    [unroll] for (int i = 0; i < 4; i++) {
        if (n4[i].a < 0.5) { float d = luma(n4[i].rgb) - lc; float w = exp(-d * d / 0.004); acc += n4[i].rgb * w; wsum += w; }
        if (d4[i].a < 0.5) { float d = luma(d4[i].rgb) - lc; float w = 0.7 * exp(-d * d / 0.004); acc += d4[i].rgb * w; wsum += w; }
    }
    float3 sm3 = lerp(c.rgb, acc / wsum, saturate(qb.z));    // bilateral: grain inside flat areas goes, outlines stay
    float lmn = lc, lmx = lc; float3 avg = float3(0,0,0); float na = 0;
    [unroll] for (int j = 0; j < 4; j++) if (n4[j].a < 0.5) { float l = luma(n4[j].rgb); lmn = min(lmn, l); lmx = max(lmx, l); avg += n4[j].rgb; na += 1; }
    if (na > 0 && qb.w > 0.001) {                             // FXAA-lite: where a clear edge passes through the pixel, blend it with its neighbours
        float contrast = lmx - lmn;
        float e = saturate((contrast - 0.10) / 0.25);
        sm3 = lerp(sm3, avg / na, e * saturate(qb.w) * 0.5);
    }
    return float4(sm3, 0);
}
float3 CatmullRom(float2 uv) {                              // 9-tap bicubic via bilinear taps (Catmull-Rom)
    float2 tex = src.xy; float2 pos = uv * tex; float2 tc = floor(pos - 0.5) + 0.5; float2 f = pos - tc; float2 f2 = f * f, f3 = f2 * f;
    float2 w0 = f2 - 0.5 * (f3 + f), w1 = 1.5 * f3 - 2.5 * f2 + 1.0, w3 = 0.5 * (f3 - f2), w2 = 1.0 - w0 - w1 - w3;
    float2 w12 = w1 + w2, tc0 = (tc - 1.0) / tex, tc3 = (tc + 2.0) / tex, tc12 = (tc + w2 / w12) / tex;
    float3 r = gameTex.SampleLevel(lin, float2(tc0.x, tc0.y), 0).rgb * (w0.x * w0.y)
             + gameTex.SampleLevel(lin, float2(tc12.x, tc0.y), 0).rgb * (w12.x * w0.y)
             + gameTex.SampleLevel(lin, float2(tc3.x, tc0.y), 0).rgb * (w3.x * w0.y)
             + gameTex.SampleLevel(lin, float2(tc0.x, tc12.y), 0).rgb * (w0.x * w12.y)
             + gameTex.SampleLevel(lin, float2(tc12.x, tc12.y), 0).rgb * (w12.x * w12.y)
             + gameTex.SampleLevel(lin, float2(tc3.x, tc12.y), 0).rgb * (w3.x * w12.y)
             + gameTex.SampleLevel(lin, float2(tc0.x, tc3.y), 0).rgb * (w0.x * w3.y)
             + gameTex.SampleLevel(lin, float2(tc12.x, tc3.y), 0).rgb * (w12.x * w3.y)
             + gameTex.SampleLevel(lin, float2(tc3.x, tc3.y), 0).rgb * (w3.x * w3.y);
    return max(r, 0.0);
}
float4 PS2(float4 pos : SV_Position) : SV_Target {          // pass 2: scaling to the window + sharpening + colour grading + cursor
    float2 q = (pos.xy - dest.xy) / dest.zw;
    if (q.x < 0 || q.y < 0 || q.x >= 1 || q.y >= 1) return float4(0,0,0,1);
    int mode = (int)(qa.x + 0.5); float3 col;
    if (mode == 0) {                                          // sharp bilinear (crisp texels, smooth edges)
        float2 s = q * src.xy - 0.5; int2 i0 = (int2)floor(s); float2 f = frac(s);
        float2 k = max(dest.zw / src.xy, 1.0); f = saturate((f - 0.5) * k + 0.5);
        float3 a = lerp(G4(i0).rgb, G4(i0 + int2(1,0)).rgb, f.x), b = lerp(G4(i0 + int2(0,1)).rgb, G4(i0 + int2(1,1)).rgb, f.x);
        col = lerp(a, b, f.y);
    } else if (mode == 1) col = CatmullRom(q);
    else col = gameTex.SampleLevel(lin, q, 0).rgb;
    if (qa.y > 0.001) {                                       // contrast-adaptive sharpening with a halo limit
        float2 px = 0.85 / src.xy;
        float3 n = gameTex.SampleLevel(lin, q + float2(0, -px.y), 0).rgb, s2 = gameTex.SampleLevel(lin, q + float2(0, px.y), 0).rgb;
        float3 e = gameTex.SampleLevel(lin, q + float2(px.x, 0), 0).rgb, w = gameTex.SampleLevel(lin, q + float2(-px.x, 0), 0).rgb;
        float3 mn = min(min(n, s2), min(e, w)), mx = max(max(n, s2), max(e, w)); mn = min(mn, col); mx = max(mx, col);
        float3 blur = (n + s2 + e + w) * 0.25;
        col = clamp(col + (col - blur) * qa.y * 2.0, mn - 0.03, mx + 0.03);
    }
    col = pow(max(col, 0.0), 1.0 / max(qa.z, 0.05));          // gamma
    col = (col - 0.5) * qa.w + 0.5;                            // contrast
    float l = luma(col); col = lerp(float3(l, l, l), col, qb.x);   // saturation
    float2 vq = q - 0.5; col *= 1.0 - qb.y * smoothstep(0.30, 0.85, length(vq * float2(1.15, 1.0)) * 1.35);   // vignette
    col = saturate(col);
    if (cur.z > 0) {                                           // software cursor drawn at the live mouse position (crisp, unfiltered)
        float2 cl = q * src.xy - cur.xy;
        if (cl.x >= 0 && cl.y >= 0 && cl.x < cur.z && cl.y < cur.w) {
            int2 ci = (int2)floor(cl);
            if (curMask.Load(int3(ci, 0)) > 0.5) { uint i = (uint)(curIdx.Load(int3(ci, 0)) * 255.0 + 0.5); col = palTex.Load(int3(i,0,0)).rgb; }
        }
    }
    return float4(col, 1);
}
)";
template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }
// ---------------------------------------------------------------- picture quality settings (remaster.ini in the game folder, re-read when it changes)
struct Quality { bool enabled = true; int scaler = 2, blend = 1; float sharp = 1.0f, gamma = 1.0f, contrast = 1.0f, sat = 1.0f, vig = 0.0f, smooth = 1.0f, fxaa = 1.0f; FILETIME mt = {}; double lastPoll = 0; } g_q;
static const char* kIniText =
    "; Cossacks remaster - picture settings. The file is read once when the game starts: restart the game after editing it.\r\n"
        "[video]\r\n"
    "; scaler: 0 = sharp bilinear (crisp pixels), 1 = bicubic Catmull-Rom (smooth, recommended), 2 = plain bilinear\r\n"
    "scaler=2\r\n"
    "; sharpness 0..1 (contrast-adaptive, halo limited)\r\n"
    "sharpness=1.00\r\n"
    "gamma=1.00\r\n"
    "contrast=1.00\r\n"
    "saturation=1.00\r\n"
    "; vignette 0..0.5 (darkening towards the screen corners)\r\n"
    "vignette=0\r\n"
    "; world smoothing 0..1: removes terrain grain, keeps outlines (interface is never touched)\r\n"
    "smooth=1.00\r\n"
    "; anti-aliasing of sprite/terrain edges 0..1\r\n"
    "fxaa=1.00\r\n"
    "; blend=1: gentle changes of the static world (fog reveal, water, light) fade smoothly between game ticks\r\n"
    "blend=1\r\n"
    "[modes]\r\n"
    "; extra resolutions offered to the game, e.g. extra=2560x1440,2048x1152 (experimental, may break the interface). Needs a game restart.\r\n"
    "extra=\r\n";
static float IniF(const char* k, float d) { char b[64]; GetPrivateProfileStringA("video", k, "", b, sizeof b, ".\\remaster.ini"); return b[0] ? (float)atof(b) : d; }
static void QualityLoad() {
    if (GetFileAttributesA("remaster.ini") == INVALID_FILE_ATTRIBUTES) { FILE* f = fopen("remaster.ini", "wb"); if (f) { fputs(kIniText, f); fclose(f); Log("wrote default remaster.ini"); } }
    WIN32_FILE_ATTRIBUTE_DATA fd; if (GetFileAttributesExA("remaster.ini", GetFileExInfoStandard, &fd)) g_q.mt = fd.ftLastWriteTime;
    g_q.scaler = (int)IniF("scaler", 2); g_q.sharp = IniF("sharpness", 1.0f); g_q.gamma = IniF("gamma", 1.0f); g_q.contrast = IniF("contrast", 1.0f); g_q.sat = IniF("saturation", 1.0f);
    g_q.vig = IniF("vignette", 0.0f); g_q.smooth = IniF("smooth", 1.0f); g_q.fxaa = IniF("fxaa", 1.0f); g_q.blend = (int)IniF("blend", 1);
    Log("quality settings: scaler %d sharp %.2f gamma %.2f contrast %.2f sat %.2f vignette %.2f smooth %.2f fxaa %.2f blend %d", g_q.scaler, g_q.sharp, g_q.gamma, g_q.contrast, g_q.sat, g_q.vig, g_q.smooth, g_q.fxaa, g_q.blend);
}
#ifdef REMASTER_DEV
// developer build only: raw dumps of the final frame / world layer (index bytes + palette) to compare "bugged" and "refocused" states offline
static void DevDumpRaw(const char* name, const BYTE* mem, int pitch, int w, int h, Pal* pal) {
    FILE* f = fopen(name, "wb"); if (!f) return; int hdr[4] = {w, h, pitch, 0x52415744}; fwrite(hdr, 4, 4, f);
    unsigned char pe[1024]; for (int i = 0; i < 256; i++) { PALETTEENTRY e = pal ? pal->e[i] : PALETTEENTRY{(BYTE)i, (BYTE)i, (BYTE)i, 0}; pe[i*4] = e.peRed; pe[i*4+1] = e.peGreen; pe[i*4+2] = e.peBlue; pe[i*4+3] = 0; } fwrite(pe, 1, 1024, f);
    for (int y = 0; y < h; y++) fwrite(mem + (size_t)y * pitch, 1, w, f); fclose(f);
}
static void DevDumpPoll(double nowMs, Surf* p) {
    static double t0 = 0, tFocus = 0; static int nA = 0, nB = 0; static bool wasFg = true;
    bool fg = GetForegroundWindow() == g_presentWnd; if (wasFg && !fg) tFocus = 0; if (!wasFg && fg) tFocus = nowMs + 4000; wasFg = fg;
    if (LayerHits() > 0 && t0 == 0) t0 = nowMs;
    const char* tag = nullptr;
    if (t0 > 0 && nA == 0 && nowMs - t0 > 20000) { nA = 1; tag = "devdump_A_bugged"; }
    else if (tFocus > 0 && nowMs > tFocus && nB < 3) { nB++; tFocus = 0; tag = nB == 1 ? "devdump_B_refocused" : "devdump_B2"; }
    if (!tag) return;
    Pal* dp = p->pal ? p->pal : g_lastPal; char fn[96];
    sprintf(fn, "%s_final.raw", tag); DevDumpRaw(fn, (const BYTE*)p->mem, p->pitch, p->w, p->h, dp);
    if (g_world.ok && g_world.buf) { sprintf(fn, "%s_world.raw", tag); DevDumpRaw(fn, g_world.buf, g_world.pitch, g_world.w, g_world.h, dp); }
    { char fm[96]; sprintf(fm, "%s_mem.bin", tag); FILE* f = fopen(fm, "wb"); if (f) { __try { fwrite((const void*)0x680000, 1, 0xA0000, f); } __except (EXCEPTION_EXECUTE_HANDLER) {} fclose(f); } }       // the game's data area (tables, palette state) to find what Alt+Tab changes
    Log("dev dump %s (%dx%d)", tag, p->w, p->h);
}
#endif
struct Gpu {
    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr; IDXGISwapChain1* sc = nullptr; ID3D11RenderTargetView* rtv = nullptr;
    ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr; ID3D11Buffer* cb = nullptr; ID3D11PixelShader *ps1 = nullptr, *psf = nullptr, *ps2 = nullptr; ID3D11Buffer* cb2 = nullptr; ID3D11SamplerState* smp = nullptr;
    ID3D11Texture2D *tG1 = nullptr, *tG2 = nullptr; ID3D11RenderTargetView *rG1 = nullptr, *rG2 = nullptr; ID3D11ShaderResourceView *sG1 = nullptr, *sG2 = nullptr;
    ID3D11Texture2D *tWorld = nullptr; ID3D11ShaderResourceView *sWorld = nullptr; bool worldValid = false; ID3D11Texture2D *tPrev = nullptr; ID3D11ShaderResourceView *sPrev = nullptr; ID3D11Texture2D *tIdx = nullptr, *tPal = nullptr, *tRgba = nullptr, *tCurI = nullptr, *tCurM = nullptr; ID3D11ShaderResourceView *sIdx = nullptr, *sPal = nullptr, *sRgba = nullptr, *sCurI = nullptr, *sCurM = nullptr; LONG curVer = 0;
    int scW = 0, scH = 0, texW = 0, texH = 0; bool needFull = true;
} g_gpu;
static bool GpuInit(HWND wnd) {
    Gpu& g = g_gpu; D3D_FEATURE_LEVEL fl; D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, want, 2, D3D11_SDK_VERSION, &g.dev, &fl, &g.ctx);
    if (FAILED(hr)) { Log("D3D11CreateDevice failed 0x%X", hr); return false; }
    IDXGIDevice1* dxd = nullptr; IDXGIAdapter* ad = nullptr; IDXGIFactory2* fac = nullptr;
    g.dev->QueryInterface(__uuidof(IDXGIDevice1), (void**)&dxd); dxd->SetMaximumFrameLatency(1); dxd->GetAdapter(&ad); ad->GetParent(__uuidof(IDXGIFactory2), (void**)&fac);
    DXGI_ADAPTER_DESC adesc; ad->GetDesc(&adesc); Log("D3D11 device ok, feature level 0x%X, adapter %ls", fl, adesc.Description);
    DXGI_SWAP_CHAIN_DESC1 sd = {}; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    hr = fac->CreateSwapChainForHwnd(g.dev, wnd, &sd, nullptr, nullptr, &g.sc);
    if (SUCCEEDED(hr)) fac->MakeWindowAssociation(wnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    SafeRelease(fac); SafeRelease(ad); SafeRelease(dxd);
    if (FAILED(hr)) { Log("CreateSwapChainForHwnd failed 0x%X", hr); return false; }
    ID3DBlob *vb = nullptr, *pb = nullptr, *err = nullptr;
    hr = D3DCompile(kHlsl, strlen(kHlsl), nullptr, nullptr, nullptr, "VS", "vs_4_0", 0, 0, &vb, &err);
    if (SUCCEEDED(hr)) hr = D3DCompile(kHlsl, strlen(kHlsl), nullptr, nullptr, nullptr, "PS", "ps_4_0", 0, 0, &pb, &err);
    if (FAILED(hr)) { Log("shader compile failed: %s", err ? (char*)err->GetBufferPointer() : "?"); return false; }
    g.dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &g.vs); g.dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &g.ps);
    vb->Release(); pb->Release();
    {   // quality pipeline: three more pixel shaders from the same source. Any failure leaves the classic single-pass path in charge.
        const char* ents[3] = {"PS1", "PSF", "PS2"}; ID3D11PixelShader** outs[3] = {&g.ps1, &g.psf, &g.ps2}; bool all = true;
        for (int i = 0; i < 3 && all; i++) {
            ID3DBlob *b2 = nullptr, *e2 = nullptr; HRESULT h2 = D3DCompile(kHlsl, strlen(kHlsl), nullptr, nullptr, nullptr, ents[i], "ps_4_0", 0, 0, &b2, &e2);
            if (FAILED(h2)) { Log("quality shader %s failed to compile: %s", ents[i], e2 ? (char*)e2->GetBufferPointer() : "?"); all = false; }
            else { g.dev->CreatePixelShader(b2->GetBufferPointer(), b2->GetBufferSize(), nullptr, outs[i]); if (!*outs[i]) all = false; }
            if (b2) b2->Release(); if (e2) e2->Release();
        }
        if (!all) { SafeRelease(g.ps1); SafeRelease(g.psf); SafeRelease(g.ps2); Log("quality pipeline unavailable, classic output only"); }
        D3D11_BUFFER_DESC qb = {}; qb.ByteWidth = 48; qb.Usage = D3D11_USAGE_DEFAULT; qb.BindFlags = D3D11_BIND_CONSTANT_BUFFER; g.dev->CreateBuffer(&qb, nullptr, &g.cb2);
        D3D11_SAMPLER_DESC sd2 = {}; sd2.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sd2.AddressU = sd2.AddressV = sd2.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd2.MaxLOD = D3D11_FLOAT32_MAX; g.dev->CreateSamplerState(&sd2, &g.smp);
        QualityLoad();
    }
    D3D11_BUFFER_DESC bd = {}; bd.ByteWidth = 80; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; g.dev->CreateBuffer(&bd, nullptr, &g.cb);
    D3D11_TEXTURE2D_DESC td = {}; td.Width = 256; td.Height = 1; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    g.dev->CreateTexture2D(&td, nullptr, &g.tPal); g.dev->CreateShaderResourceView(g.tPal, nullptr, &g.sPal);
    td.Width = CUR_W; td.Height = CUR_H; td.Format = DXGI_FORMAT_R8_UNORM;     // cursor image + mask
    g.dev->CreateTexture2D(&td, nullptr, &g.tCurI); g.dev->CreateShaderResourceView(g.tCurI, nullptr, &g.sCurI);
    g.dev->CreateTexture2D(&td, nullptr, &g.tCurM); g.dev->CreateShaderResourceView(g.tCurM, nullptr, &g.sCurM);
    return true;
}
static void GpuResizeTextures(int w, int h) {
    Gpu& g = g_gpu; SafeRelease(g.rG1); SafeRelease(g.sG1); SafeRelease(g.tG1); SafeRelease(g.rG2); SafeRelease(g.sG2); SafeRelease(g.tG2);
    SafeRelease(g.sWorld); SafeRelease(g.tWorld); g.worldValid = false; SafeRelease(g.sPrev); SafeRelease(g.tPrev); SafeRelease(g.sIdx); SafeRelease(g.tIdx); SafeRelease(g.sRgba); SafeRelease(g.tRgba);
    D3D11_TEXTURE2D_DESC td = {}; td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.Format = DXGI_FORMAT_R8_UNORM; g.dev->CreateTexture2D(&td, nullptr, &g.tIdx); g.dev->CreateShaderResourceView(g.tIdx, nullptr, &g.sIdx);
    g.dev->CreateTexture2D(&td, nullptr, &g.tPrev); g.dev->CreateShaderResourceView(g.tPrev, nullptr, &g.sPrev);
    g.dev->CreateTexture2D(&td, nullptr, &g.tWorld); g.dev->CreateShaderResourceView(g.tWorld, nullptr, &g.sWorld);
    if (g.ps2) {                                                                                  // game-resolution targets of the quality pipeline
        D3D11_TEXTURE2D_DESC gd = td; gd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; gd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        if (SUCCEEDED(g.dev->CreateTexture2D(&gd, nullptr, &g.tG1))) { g.dev->CreateShaderResourceView(g.tG1, nullptr, &g.sG1); g.dev->CreateRenderTargetView(g.tG1, nullptr, &g.rG1); }
        if (SUCCEEDED(g.dev->CreateTexture2D(&gd, nullptr, &g.tG2))) { g.dev->CreateShaderResourceView(g.tG2, nullptr, &g.sG2); g.dev->CreateRenderTargetView(g.tG2, nullptr, &g.rG2); }
    }
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; g.dev->CreateTexture2D(&td, nullptr, &g.tRgba); g.dev->CreateShaderResourceView(g.tRgba, nullptr, &g.sRgba);
    g.texW = w; g.texH = h; g.needFull = true;
}

// ---------------------------------------------------------------- smooth scrolling (presenter-side motion compensation)
// The game scrolls the map in discrete steps once per game tick (~21 Hz). For each new game frame we measure the global
// world translation against the previous frame (row/column luminance profiles + block verification) and then the GPU shows
// the world sliding smoothly between the two frames at the display rate. HUD pixels (unchanged between frames) stay put.
struct Smooth { u8 *prev = nullptr, *cur = nullptr; size_t sz = 0; int w = 0, h = 0, pitch = 0; bool layered = false; int vx = 0, vy = 0; double tArrive = 0, tEst = 48; bool have = false, enabled = true; int frames = 0; } g_sm;
#ifdef REMASTER_DEV
static const int kCamChecked0 = 0;                          // developer build: the camera model is checked against the image for 300 frames first
#else
static const int kCamChecked0 = 300;                        // release: the camera model is trusted from the start (it agreed with the image analysis in every test), no image analysis
#endif
static struct CamShift { bool enabled = true, have = false; int checked = kCamChecked0, agree = 0, logged = 0, pcx = 0, pcy = 0; } g_camShift;
static int g_camPX, g_camPY;     // previous camera variables (for the log)
static int EstimateAxis(const int* a, const int* b, int lo, int hi, int R, int len, long long* c0out) {   // a = previous profile, b = current; best s with b[x] ~ a[x-s]
    long long best = -1; int bs = 0; long long c0 = 0;
    for (int s = -R; s <= R; s++) {
        long long c = 0; int n = 0;
        for (int x = lo + R; x < hi - R; x++) { int d = b[x] - a[x - s]; c += d < 0 ? -d : d; n++; }
        c = c * 1000 / (n ? n : 1);
        if (s == 0) c0 = c;
        if (best < 0 || c < best) { best = c; bs = s; }
    }
    *c0out = c0;
    if (bs != 0 && best * 10 > c0 * 7) bs = 0;           // not clearly better than "no motion"
    return bs;
}
static void SmoothNewFrame(Surf* p, double now, bool useWorld) {
    if (!g_sm.enabled) return;
    const u8* srcBuf = useWorld ? g_world.buf : p->mem; int spitch = useWorld ? g_world.pitch : p->pitch;
    size_t sz = (size_t)spitch * p->h;
    if (g_sm.sz != sz || g_sm.w != p->w || g_sm.h != p->h || g_sm.layered != useWorld) { g_sm.layered = useWorld; g_sm.pitch = spitch; delete[] g_sm.prev; delete[] g_sm.cur; g_sm.prev = new u8[sz]; g_sm.cur = new u8[sz]; g_sm.sz = sz; g_sm.w = p->w; g_sm.h = p->h; g_sm.have = false; g_sm.frames = 0; memset(g_sm.cur, 0, sz); }
    u8* t = g_sm.prev; g_sm.prev = g_sm.cur; g_sm.cur = t; memcpy(g_sm.cur, srcBuf, sz);        // prev <- old cur, cur <- new frame
    double iv = now - g_sm.tArrive; g_sm.tArrive = now;
    if (g_sm.frames > 0 && iv > 15 && iv < 150) g_sm.tEst = g_sm.tEst * 0.8 + iv * 0.2;
    g_sm.frames++; g_sm.vx = g_sm.vy = 0;
    if (g_sm.frames < 2 || p->bpp != 8 || iv > 150) { g_sm.have = g_sm.frames >= 2; if (useWorld) { g_camShift.have = true; g_camShift.pcx = g_world.camX; g_camShift.pcy = g_world.camY; } return; }
    g_sm.have = true;
    if (useWorld && g_camShift.enabled && g_camShift.have && g_camShift.checked >= 300) {       // the camera model has proven itself: exact shift, no image analysis
        int cx = g_world.camX, cy = g_world.camY, mx = -(cx - g_camShift.pcx) * 32, my = -(cy - g_camShift.pcy) * 16; g_camShift.pcx = cx; g_camShift.pcy = cy;
        if (abs(mx) <= 320 && abs(my) <= 192) { g_sm.vx = mx; g_sm.vy = my; } return;
    }
    Pal* pal = p->pal ? p->pal : g_lastPal; int lum[256]; for (int i = 0; i < 256; i++) { PALETTEENTRY e = pal ? pal->e[i] : PALETTEENTRY{(BYTE)i, (BYTE)i, (BYTE)i, 0}; lum[i] = (e.peRed * 30 + e.peGreen * 59 + e.peBlue * 11) / 100; }
    int W = p->w, H = p->h, pitch = spitch, x0 = W * 15 / 100, x1 = W * 85 / 100, y0 = H * 12 / 100, y1 = H * 72 / 100;
    if (useWorld) { x0 = W * 5 / 100; x1 = W * 95 / 100; y0 = H * 6 / 100; y1 = H * 94 / 100; }          // pure world layer: no interface to avoid
    static std::vector<int> cp, cc, rp, rc; cp.assign(W, 0); cc.assign(W, 0); rp.assign(H, 0); rc.assign(H, 0);
    for (int y = y0; y < y1; y++) { const u8* a = g_sm.prev + (size_t)y * pitch; const u8* b = g_sm.cur + (size_t)y * pitch; int ra = 0, rb = 0;
        for (int x = x0; x < x1; x++) { int la = lum[a[x]], lb = lum[b[x]]; cp[x] += la; cc[x] += lb; ra += la; rb += lb; } rp[y] = ra; rc[y] = rb; }
    int R = 96; long long c0x, c0y;
    int dx = EstimateAxis(cp.data(), cc.data(), x0, x1, R, W, &c0x), dy = EstimateAxis(rp.data(), rc.data(), y0, y1, R, H, &c0y);
    // The game moves the camera in whole cells (32 x 16 px): world content shifts by exactly -(delta camera) * (32,16) between two frames.
    // That is exact and works on flat terrain where the image-based estimate has nothing to lock onto. The estimate is kept as a self-check
    // (first 300 frames) and as a fallback should the camera model ever disagree with the pixels.
    if (useWorld && g_camShift.enabled) {
        bool& have = g_camShift.have; int& pcx = g_camShift.pcx; int& pcy = g_camShift.pcy; int cx = g_world.camX, cy = g_world.camY;
        if (have) {
            int mx = -(cx - pcx) * 32, my = -(cy - pcy) * 16; bool plausible = abs(mx) <= 320 && abs(my) <= 192;
            bool estimated = (dx != 0 || dy != 0);
            if (g_camShift.checked < 300 && (mx || my || estimated)) {
                g_camShift.checked++; bool agree = (mx == dx && my == dy); if (agree) g_camShift.agree++;
                else if (g_camShift.logged++ < 25) Log("camshift check: camera model (%d,%d) vs image estimate (%d,%d) [cam %d,%d -> %d,%d]", mx, my, dx, dy, pcx, pcy, cx, cy);
                if (g_camShift.checked == 300) { Log("camshift self-check: %d of 300 agree", g_camShift.agree); if (g_camShift.agree < 150) { g_camShift.enabled = false; Log("camshift: disagreement, falling back to image-based estimate"); } }
            }
            pcx = cx; pcy = cy;
            if (g_camShift.enabled) {
                bool disagree = g_camShift.checked <= 300 && estimated && !(mx == dx && my == dy);      // while the model is on trial the verified image estimate wins
                if (!disagree) { if (plausible) { g_sm.vx = mx; g_sm.vy = my; } else { g_sm.vx = g_sm.vy = 0; } return; }
            }
        } else { have = true; pcx = cx; pcy = cy; if (g_camShift.enabled) return; }
    }
    if (dx == 0 && dy == 0) return;
    // verify with blocks: the shifted previous frame must match the current one clearly better than the unshifted one
    long long sad0 = 0, sadV = 0; int nb = 0;
    for (int by = y0 + 8; by + 24 < y1; by += 56) for (int bx = x0 + 8; bx + 24 < x1; bx += 56) {
        int px = bx - dx, py = by - dy; if (px < 0 || py < 0 || px + 16 > W || py + 16 > H) continue;
        for (int yy = 0; yy < 16; yy++) for (int xx = 0; xx < 16; xx++) {
            int c = lum[g_sm.cur[(size_t)(by + yy) * pitch + bx + xx]];
            int a0 = lum[g_sm.prev[(size_t)(by + yy) * pitch + bx + xx]], av = lum[g_sm.prev[(size_t)(py + yy) * pitch + px + xx]];
            sad0 += abs(c - a0); sadV += abs(c - av); }
        nb++;
    }
    if (nb < 4) return;
    double m0 = sad0 / (nb * 256.0), mV = sadV / (nb * 256.0);
    if (mV < m0 * 0.6 && mV < 25) { g_sm.vx = dx; g_sm.vy = dy; }
    static int logged; if ((g_sm.vx || g_sm.vy) && (logged++ < 40 || logged % 50 == 0)) Log("smooth: world shift (%d,%d) per tick (SAD shifted %.1f vs still %.1f), tick %.1f ms | camera vars [AF5E00]=%d [AF5E04]=%d (d %d,%d) subcell [1010544]=%d [1010548]=%d", g_sm.vx, g_sm.vy, mV, m0, g_sm.tEst, GI(0xAF5E00), GI(0xAF5E04), GI(0xAF5E00) - g_camPX, GI(0xAF5E04) - g_camPY, GI(0x1010544), GI(0x1010548));
    g_camPX = GI(0xAF5E00); g_camPY = GI(0xAF5E04);
}
static int g_presDump = 0, g_presDumpIdx = 0, g_presTick = 0;   // readback of the PRESENTED image (top strip) for N consecutive presents
static double g_prepMs;
static double Qpc() { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return c.QuadPart * 1000.0 / f.QuadPart; }
#include "gpuworld.inc"
static void GpuFrame(HWND wnd, Surf* p, bool upload, const RECT* dirty, bool newFrame, float smF, bool useWorld) {
    double tStart = Qpc(); Gpu& g = g_gpu; RECT rc; GetClientRect(wnd, &rc); int cw = rc.right, ch = rc.bottom; if (cw <= 0 || ch <= 0) { Sleep(16); return; }
    if (!g.rtv || g.scW != cw || g.scH != ch) {
        SafeRelease(g.rtv); g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
        if (g.scW) g.sc->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
        ID3D11Texture2D* bb = nullptr; g.sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb); g.dev->CreateRenderTargetView(bb, nullptr, &g.rtv); bb->Release();
        DXGI_SWAP_CHAIN_DESC1 d; g.sc->GetDesc1(&d); g.scW = d.Width; g.scH = d.Height; cw = g.scW; ch = g.scH; Log("swap chain %dx%d", cw, ch);
    }
    if (g.texW != p->w || g.texH != p->h) GpuResizeTextures(p->w, p->h);
    int mode = 1;
    if (p->bpp == 8) {
        mode = 0;
        if (newFrame && g.tPrev && !g.needFull) {
            if (useWorld && g.tWorld) {                                                           // layered: previous world layer <- old world layer, new world layer upload
                if (g.worldValid) g.ctx->CopyResource(g.tPrev, g.tWorld);
                const u8* baseBuf = g_world.buf; bool gwOk = false;
#ifdef REMASTER_GPUW
                gwOk = GwRun(g, p->w, p->h, g_world.seq, p->pal ? p->pal : g_lastPal);       // the world layer composed on the GPU
                g_gwRgbPrevGood = gwOk && g_gwRgbOn; g_gwRgbOn = gwOk;                              // scroll interpolation needs the previous tick in true colour too
#endif
                if (!gwOk) g.ctx->UpdateSubresource(g.tWorld, 0, nullptr, baseBuf, g_world.pitch, 0); g.worldValid = true;
            } else { g.worldValid = false; g.ctx->CopyResource(g.tPrev, g.tIdx);
#ifdef REMASTER_GPUW
                g_gwRgbOn = false; g_gwRgbPrevGood = false;
#endif
            }               // fallback: previous final frame
        }
        if (g.needFull || (upload && !dirty)) { g.ctx->UpdateSubresource(g.tIdx, 0, nullptr, p->mem, p->pitch, 0); g.needFull = false; }
        else if (upload && dirty) {
            D3D11_BOX bx = {(UINT)max(0L, dirty->left), (UINT)max(0L, dirty->top), 0, (UINT)min((LONG)p->w, dirty->right), (UINT)min((LONG)p->h, dirty->bottom), 1};
            if (bx.right > bx.left && bx.bottom > bx.top) g.ctx->UpdateSubresource(g.tIdx, 0, &bx, p->mem + (size_t)bx.top * p->pitch + bx.left, p->pitch, 0);
        }
        Pal* pal = p->pal ? p->pal : g_lastPal; DWORD pe[256];
        for (int i = 0; i < 256; i++) { PALETTEENTRY e = pal ? pal->e[i] : PALETTEENTRY{(BYTE)i, (BYTE)i, (BYTE)i, 0}; pe[i] = 0xFF000000 | (e.peBlue << 16) | (e.peGreen << 8) | e.peRed; }
        g.ctx->UpdateSubresource(g.tPal, 0, nullptr, pe, 1024, 0);
    } else {
        if (g_bgraW != p->w || g_bgraH != p->h) { delete[] g_bgra; g_bgra = new DWORD[p->w * p->h]; g_bgraW = p->w; g_bgraH = p->h; }
        for (int y = 0; y < p->h; y++) { const u8* src = p->mem + (size_t)y * p->pitch; DWORD* dst = g_bgra + y * p->w;
            if (p->bpp == 16) { const WORD* s16 = (const WORD*)src; bool is565 = p->pf.gMask == 0x07E0;
                for (int x = 0; x < p->w; x++) { WORD v = s16[x]; u32 r, gg, b; if (is565) { r = (v >> 11) & 31; gg = (v >> 5) & 63; b = v & 31; r = r * 255 / 31; gg = gg * 255 / 63; b = b * 255 / 31; } else { r = (v >> 10) & 31; gg = (v >> 5) & 31; b = v & 31; r = r * 255 / 31; gg = gg * 255 / 31; b = b * 255 / 31; } dst[x] = (r << 16) | (gg << 8) | b; } }
            else memcpy(dst, src, p->w * 4); }
        g.ctx->UpdateSubresource(g.tRgba, 0, nullptr, g_bgra, p->w * 4, 0);
    }
    int ox, oy, dw, dh; GetDest(wnd, p->w, p->h, ox, oy, dw, dh);
    float cur[4] = {0, 0, 0, 0};                                              // software cursor overlay at the live OS cursor position
    if (g_cur.visible && g_cur.version && mode == 0) {
        if (g.curVer != g_cur.version) { EnterCriticalSection(&g_cur.cs); g.ctx->UpdateSubresource(g.tCurI, 0, nullptr, g_cur.img, CUR_W, 0); g.ctx->UpdateSubresource(g.tCurM, 0, nullptr, g_cur.mask, CUR_W, 0); g.curVer = g_cur.version; LeaveCriticalSection(&g_cur.cs); }
        POINT pt; if (GetCursorPos(&pt) && ScreenToClient(wnd, &pt) && dw > 0 && dh > 0) {
            float gx = (pt.x - ox) * (float)p->w / dw, gy = (pt.y - oy) * (float)p->h / dh;      // game-space mouse position
            cur[0] = floorf(gx) - g_cur.hotX - CUR_ORG; cur[1] = floorf(gy) - g_cur.hotY - CUR_ORG; cur[2] = CUR_W; cur[3] = CUR_H;
        }
    }
    bool maskOk = g.worldValid && g_sm.layered && mode == 0 && !g.needFull;                         // a world layer exists: the interface mask is known
    bool smOn = g_sm.enabled && g_sm.have && (g_sm.vx || g_sm.vy) && mode == 0 && !g.needFull;
    float layOn = (smOn && g.worldValid && g_sm.layered) ? 1.0f : 0.0f;
    bool rgbOn = false;
#ifdef REMASTER_GPUW
    rgbOn = g_gwRgbOn && g_gw.rgbValid && g_gw.frames >= 2 && maskOk && (layOn < 0.5f || g_gwRgbPrevGood);
#endif
    float cbv[20] = {(float)p->w, (float)p->h, (float)mode, 0, (float)ox, (float)oy, (float)dw, (float)dh, cur[0], cur[1], cur[2], cur[3], (float)g_sm.vx, (float)g_sm.vy, smF, smOn ? 1.0f : 0.0f, layOn, 0.0f, maskOk ? 1.0f : 0.0f, rgbOn ? 1.0f : 0.0f}; g.ctx->UpdateSubresource(g.cb, 0, nullptr, cbv, 0, 0);
    bool qpipe = g_q.enabled && g.ps1 && g.psf && g.ps2 && g.rG1 && g.rG2 && g.cb2 && g.smp;
    float qd[12] = {(float)g_q.scaler, g_q.sharp, g_q.gamma, g_q.contrast, g_q.sat, g_q.vig, maskOk ? g_q.smooth : 0.0f, maskOk ? g_q.fxaa : 0.0f, qpipe ? (float)g_q.blend : 0.0f, 0, 0, 0};
    if (g.cb2) g.ctx->UpdateSubresource(g.cb2, 0, nullptr, qd, 0, 0);
    ID3D11ShaderResourceView* srvs[9] = {g.sIdx, g.sPal, g.sRgba, g.sCurI, g.sCurM, g.sPrev, g.sWorld, nullptr, nullptr};
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); g.ctx->IASetInputLayout(nullptr); g.ctx->VSSetShader(g.vs, nullptr, 0);
    ID3D11Buffer* cbs[2] = {g.cb, g.cb2}; g.ctx->PSSetConstantBuffers(0, 2, cbs);
#ifdef REMASTER_GPUW
    { ID3D11ShaderResourceView* rg[2] = {g_gw.sWRgb, g_gw.sPRgb}; g.ctx->PSSetShaderResources(10, 2, rg); }
#endif
    if (!qpipe) {                                                                    // classic single pass
        D3D11_VIEWPORT vp = {0, 0, (float)g.scW, (float)g.scH, 0, 1}; g.ctx->RSSetViewports(1, &vp); g.ctx->OMSetRenderTargets(1, &g.rtv, nullptr);
        g.ctx->PSSetShader(g.ps, nullptr, 0); g.ctx->PSSetShaderResources(0, 9, srvs); g.ctx->Draw(3, 0);
    } else {
        ID3D11ShaderResourceView* nul = nullptr; ID3D11RenderTargetView* nrt = nullptr;
        // pass 1: layers -> game-resolution RGBA (+ interface mask in alpha)
        g.ctx->PSSetShaderResources(9, 1, &nul); g.ctx->OMSetRenderTargets(1, &g.rG1, nullptr);
        D3D11_VIEWPORT v1 = {0, 0, (float)p->w, (float)p->h, 0, 1}; g.ctx->RSSetViewports(1, &v1);
        g.ctx->PSSetShader(g.ps1, nullptr, 0); g.ctx->PSSetShaderResources(0, 9, srvs); g.ctx->Draw(3, 0);
        ID3D11ShaderResourceView* gsrv = g.sG1;
        // pass 1b: smoothing / anti-aliasing of the world (only when asked for and the interface mask is known)
        if (maskOk && (g_q.smooth > 0.001f || g_q.fxaa > 0.001f)) {
            g.ctx->OMSetRenderTargets(1, &g.rG2, nullptr); g.ctx->PSSetShaderResources(9, 1, &g.sG1); g.ctx->PSSetShader(g.psf, nullptr, 0); g.ctx->Draw(3, 0); gsrv = g.sG2;
        }
        // pass 2: scale to the window, sharpen, grade, cursor
        g.ctx->OMSetRenderTargets(1, &nrt, nullptr); g.ctx->PSSetShaderResources(9, 1, &nul);
        D3D11_VIEWPORT v2 = {0, 0, (float)g.scW, (float)g.scH, 0, 1}; g.ctx->RSSetViewports(1, &v2); g.ctx->OMSetRenderTargets(1, &g.rtv, nullptr);
        g.ctx->PSSetSamplers(0, 1, &g.smp); g.ctx->PSSetShader(g.ps2, nullptr, 0); g.ctx->PSSetShaderResources(9, 1, &gsrv); g.ctx->Draw(3, 0);
        g.ctx->PSSetShaderResources(9, 1, &nul);
    }
    if (g_presDump > 0 && (g_presTick++ % 4) == 0) {                                      // top strip of what is really shown (after the shader)
        ID3D11Texture2D* bb = nullptr; g.sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
        if (bb) { D3D11_TEXTURE2D_DESC d; bb->GetDesc(&d); d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0; ID3D11Texture2D* st = nullptr;
            if (SUCCEEDED(g.dev->CreateTexture2D(&d, nullptr, &st))) { g.ctx->CopyResource(st, bb); D3D11_MAPPED_SUBRESOURCE m;
                if (SUCCEEDED(g.ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) { int rows = 90 < (int)d.Height ? 90 : (int)d.Height; u8* rgb = (u8*)malloc((size_t)d.Width * rows * 3);
                    for (int y = 0; y < rows; y++) { const u8* sp = (const u8*)m.pData + (size_t)y * m.RowPitch; u8* dp = rgb + (size_t)y * d.Width * 3; for (UINT x = 0; x < d.Width; x++) { dp[x * 3] = sp[x * 4 + 2]; dp[x * 3 + 1] = sp[x * 4 + 1]; dp[x * 3 + 2] = sp[x * 4]; } }
                    char fn[64]; sprintf(fn, "presented%d_f%02d.png", g_presDumpIdx, (int)(smF * 100)); WritePng(fn, d.Width, rows, rgb); free(rgb); g.ctx->Unmap(st, 0); }
                st->Release(); }
            bb->Release(); }
        g_presDump--;
    }
    g_prepMs = Qpc() - tStart; g.sc->Present(1, 0); g_lastPresentMs = Qpc();
}
static DWORD WINAPI PresentThread(LPVOID) {
    while (!g_presentWnd || !g_primary) Sleep(5);   // wait for the game window + primary surface
    if (!GpuInit(g_presentWnd)) { Log("falling back to GDI presenter"); return GdiThread(nullptr); }
    static u8* shadow = nullptr; static size_t shadowSz = 0; g_sm.enabled = true;
    double ivMax = 0, lastNewT = 0; int iv60 = 0, iv100 = 0, newCnt = 0, scrollDump = 0; static int scrollFrames = 0;
    double t0 = Qpc(), lastChange = t0, maxGap = 0, sumPrep = 0, maxPrep = 0, maxFrame = 0, lastFrame = t0; int frames = 0, changed = 0, slow = 0; static double gaps[2048]; int ng = 0;
    for (;;) {
        TracePoll();
        Surf* p = g_primary; if (!p || !p->mem) { Sleep(5); continue; }
        size_t sz = (size_t)p->pitch * p->h; if (shadowSz != sz) { delete[] shadow; shadow = new u8[sz]; shadowSz = sz; memset(shadow, 0, sz); }
        double now = Qpc(); bool dirtyMode = g_patchCopyActive && !g_copyPtrMismatch && p->bpp == 8; RECT dr = {0, 0, 0, 0}; bool upload;
        if (dirtyMode) { upload = TakeDirty(&dr); if (upload) { changed++; double gap = now - lastChange; lastChange = now; if (gap > maxGap) maxGap = gap; if (ng < 2048) gaps[ng++] = gap; } }
        else { upload = true; if (memcmp(shadow, p->mem, sz) != 0) { memcpy(shadow, p->mem, sz); changed++; double gap = now - lastChange; lastChange = now; if (gap > maxGap) maxGap = gap; if (ng < 2048) gaps[ng++] = gap; } }
        bool newFrame = false, useWorld = false; float smF = 1.0f;
#ifdef REMASTER_DEV
        DevDumpPoll(now, p);
#endif
        if (dirtyMode && upload && (double)(dr.right - dr.left) * (dr.bottom - dr.top) >= 0.9 * p->w * p->h) { newFrame = true; static LONG lastSeq; LONG sq = g_world.seq; useWorld = g_world.ok && sq != lastSeq; lastSeq = sq;
            SmoothNewFrame(p, now, useWorld);
            if (lastNewT > 0) { double ivx = now - lastNewT; if (ivx > ivMax) ivMax = ivx; if (ivx > 60) iv60++; if (ivx > 100) iv100++; } lastNewT = now; newCnt++;
            static bool dumpOn = false;
            if (dumpOn && useWorld && (g_sm.vx || g_sm.vy) && ++scrollFrames % 25 == 0 && scrollDump < 4) {          // evidence for interface/world mix-ups while scrolling
                char fn[64]; Pal* dp = p->pal ? p->pal : g_lastPal; int n = ++scrollDump; int W = p->w, H = p->h;
                sprintf(fn, "dump%d_final_half.png", n); DumpIndexed(fn, p->mem, p->pitch, W, H, 0, 0, W, H, dp, 2);
                sprintf(fn, "dump%d_final_top.png", n); DumpIndexed(fn, p->mem, p->pitch, W, H, 0, 0, W, 80, dp, 1);
                sprintf(fn, "dump%d_world_top.png", n); DumpIndexed(fn, g_world.buf, g_world.pitch, W, H, 0, 0, W, 80, dp, 1);
                g_presDump = 6; g_presDumpIdx = n; g_presTick = 0; Log("dumped scroll evidence dump%d_*.png + presented%d_f*.png (shift %d,%d)", n, n, g_sm.vx, g_sm.vy);
            } }
        if (g_sm.have) { double f = (now - g_sm.tArrive) / g_sm.tEst; smF = (float)(f < 0 ? 0 : f > 1 ? 1 : f); }
        GpuFrame(g_presentWnd, p, upload, dirtyMode ? &dr : nullptr, newFrame, smF, useWorld); frames++; sumPrep += g_prepMs; if (g_prepMs > maxPrep) maxPrep = g_prepMs;
        double n2 = Qpc(); double fd = n2 - lastFrame; lastFrame = n2; if (fd > maxFrame) maxFrame = fd; if (fd > 9.0) slow++;
        { static int hl; if (fd > 14.0 && hl++ < 40) Log("PRESENT hitch %.1f ms (prep %.2f ms, newFrame %d, shift %d,%d)", fd, g_prepMs, (int)newFrame, g_sm.vx, g_sm.vy); }   // > ~1.5 refresh at 165 Hz
        if (n2 - t0 >= 2000) {
            double p95 = 0; if (ng > 1) { for (int i = 1; i < ng; i++) { double v = gaps[i]; int j = i; while (j > 0 && gaps[j - 1] > v) { gaps[j] = gaps[j - 1]; j--; } gaps[j] = v; } p95 = gaps[(int)(ng * 0.95)]; }
            Log("%dx%d present %.1f Hz (slow frames %d, max %.1f ms) prep avg %.2f max %.2f ms | uploads %.1f/s gap p95 %.1f max %.1f ms | GAME FRAMES %.1f/s | tick interval max %.1f ms (>60: %d, >100: %d) game work max %.1f ms (>40: %ld of %ld)",
                p->w, p->h, frames * 1000.0 / (n2 - t0), slow, maxFrame, sumPrep / (frames ? frames : 1), maxPrep, changed * 1000.0 / (n2 - t0), p95, maxGap, InterlockedExchange(&g_gameFrames, 0) * 1000.0 / (n2 - t0), ivMax, iv60, iv100, g_workMax, InterlockedExchange(&g_workOver40, 0), InterlockedExchange(&g_workTicks, 0));
            ivMax = 0; iv60 = iv100 = 0; g_workMax = 0;
            t0 = n2; frames = changed = slow = ng = 0; maxGap = sumPrep = maxPrep = maxFrame = 0;
        }
    }
}
