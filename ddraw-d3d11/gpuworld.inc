// ---------------------------------------------------------------- variant B, stage 2: the world layer composed on the GPU (developer build)
// Input: GpuWorldJob from sprites.inc (base frame, resolved sprite records with per-tile lists, atlas of black-box sprites, tables, fog cells).
//   pass 1 (pixel shader, one pixel = one game pixel): base index -> for every record of the 16x16 tile in draw order: opaque index or table lookup T[value][current]
//   pass 2: fog of war, closed form of 004749B1 (bilinear level over the 32x16 cell -> lookup table 6F51B0)
// The result (R8 index image = the world layer after fog) replaces the upload of the game's own snapshot; a read-back comparison against that snapshot is logged.
// True colour: next to the index image every pass also produces an RGB image. Shadow / smoke / fire tables (index -> index) are replaced by an affine RGB model
// per table (out = in * scale + offset, fitted on the palette colours), the fog by the same model per fog level, interpolated between levels: no palette steps.
// A table that does not fit the model well keeps its exact palette lookup.
#ifdef REMASTER_GPUW
static const char* kGwHlsl = R"HLSL(
cbuffer P : register(b0) { uint4 p0; int4 p1; };       // p0: tilesW, fogOn, cols, rows ; p1: ox, oy, 0, 0
Texture2D<uint> baseTex : register(t0);
Texture2D<uint> atlas : register(t1);
Texture2D<uint> tbl : register(t2);
Buffer<int4> recA : register(t3);                       // bx, by, w, h
Buffer<int4> recB : register(t4);                       // ax, ay, 0, 0
Buffer<int4> recC : register(t5);                       // clip x0, x1, y0, y1
Buffer<uint> tileOff : register(t6);
Buffer<uint> tileIdx : register(t7);
Texture2D<float4> pal : register(t8);
Buffer<float4> mdlS : register(t9);                      // per table: scale rgb, w = 1 when the affine model fits
Buffer<float4> mdlO : register(t10);                     // per table: offset rgb
struct POut { float4 i : SV_Target0; float4 c : SV_Target1; };
POut PSC(float4 pos : SV_Position) {
    int px = (int)pos.x, py = (int)pos.y;
    uint v = baseTex.Load(int3(px, py, 0)).r; float3 col = pal.Load(int3(v, 0, 0)).rgb; float fl = 0;                  // fl: the pixel was touched by a dithered (checkerboard) effect
    uint tile = (uint)(py >> 4) * p0.x + (uint)(px >> 4);
    uint s = tileOff[tile], e = tileOff[tile + 1];
    for (uint i = s; i < e; i++) {
        uint ri = tileIdx[i]; int4 a = recA[ri]; int4 c = recC[ri];
        if (px < c.x || px > c.y || py < c.z || py > c.w) continue;
        int lx = px - a.x, ly = py - a.y; if (lx < 0 || ly < 0 || lx >= a.z || ly >= a.w) continue;
        int4 b = recB[ri]; uint t = atlas.Load(int3(b.x + lx, b.y + ly, 0)).r; uint k = t >> 14; if (k == 0) continue;
        uint val = t & 0x3FFF;
        if (k == 1) { v = val; col = pal.Load(int3(val, 0, 0)).rgb; fl = 0; }
        else { if (val & 0x2000) fl = 1; val &= 0xFFF; uint n = tbl.Load(int3(v, val, 0)).r; float4 ms = mdlS[val]; if (ms.w > 0.5) col = saturate(col * ms.rgb + mdlO[val].rgb); else col = pal.Load(int3(n, 0, 0)).rgb; v = n; }
    }
    POut o; o.i = float4(v / 255.0, 0, 0, 0); o.c = float4(col, fl); return o;
}
Texture2D<uint> wTex : register(t0);
Texture2D<uint> cellTex : register(t1);
Texture2D<uint> lutTex : register(t2);
Texture2D<float4> pal2 : register(t3);
Buffer<float4> fogS : register(t4);                      // per fog level: scale rgb, w = 1 when the model fits
Buffer<float4> fogO : register(t5);
Texture2D<float4> colTex : register(t6);
POut Mk(uint idx, float3 col, float fl) { POut o; o.i = float4(idx / 255.0, 0, 0, 0); o.c = float4(col, fl); return o; }
POut PSG(float4 pos : SV_Position) {
    int px = (int)pos.x, py = (int)pos.y; uint pix = wTex.Load(int3(px, py, 0)).r; float4 c0a = colTex.Load(int3(px, py, 0)); float3 c0 = c0a.rgb; float fl = c0a.a;
    if (p0.y == 0 || px < p1.x || py < p1.y) return Mk(pix, c0, fl);
    int c = (px - p1.x) >> 5, r = (py - p1.y) >> 4; if (c >= (int)p0.z || r >= (int)p0.w) return Mk(pix, c0, fl);
    uint cl = cellTex.Load(int3(c, r, 0)).r; int c0l = cl & 255, c1 = (cl >> 8) & 255, c2 = (cl >> 16) & 255, c3 = (cl >> 24) & 255;
    if (c0l <= 0x3F && c1 <= 0x3F && c2 <= 0x3F && c3 <= 0x3F) return Mk(pix, c0, fl);
    if (c0l >= 0x60 && c1 >= 0x60 && c2 >= 0x60 && c3 >= 0x60) return Mk(0, float3(0, 0, 0), 0);
    int u = (px - p1.x) & 31, v = (py - p1.y) & 15;
    int A = c0l << 16, B = c1 << 16, C = c2 << 16, D = c3 << 16; int dA = (C - A) >> 4, dd = (A + D - C - B) >> 9, s0 = (B - A) >> 5;
    int e = A + v * dA + u * (s0 + v * dd);
    uint idx = (u < 4) ? ((uint)((e >> 8) & ~0xFF) | pix) : ((((uint)e) >> 8) & 0xFFFFFF00u) | pix;
    uint lv = idx >> 8; if (lv >= 128) return Mk(0, float3(0, 0, 0), 0);
    uint o = lutTex.Load(int3(idx & 255, lv, 0)).r;
    float lvf = max((float)e, 0.0) * (1.0 / 65536.0); uint l0 = min((uint)lvf, 127u), l1 = min(l0 + 1u, 127u); float f = frac(lvf);
    float4 s0v = fogS[l0], s1v = fogS[l1]; float3 col;
    if (s0v.w > 0.5 && s1v.w > 0.5) col = lerp(saturate(c0 * s0v.rgb + fogO[l0].rgb), saturate(c0 * s1v.rgb + fogO[l1].rgb), f);
    else col = pal2.Load(int3(o, 0, 0)).rgb;
    return Mk(o, col, fl);
}
)HLSL";
#ifdef REMASTER_DEV
static const bool g_gwDevLog = true;
#else
static const bool g_gwDevLog = false;
#endif
static bool g_gwRgbOn = false, g_gwRgbPrevGood = false;                           // the last world layer carries true colour (GpuFrame sets it per frame)
struct AffFit { float s[3], o[3]; bool ok; };
// out = in * s + o per channel, least squares over the 256 palette colours; ok when the residual is small (the table is really a darkening / blend)
static AffFit FitAffine(const float (*pr)[3], const unsigned char* map) {
    AffFit f = {}; double err = 0;
    for (int c = 0; c < 3; c++) {
        double sx = 0, sy = 0, sxx = 0, sxy = 0; for (int v = 0; v < 256; v++) { double x = pr[v][c], y = pr[map[v]][c]; sx += x; sy += y; sxx += x * x; sxy += x * y; }
        double den = 256 * sxx - sx * sx, sl = den > 1e-6 ? (256 * sxy - sx * sy) / den : 0, of = (sy - sl * sx) / 256; f.s[c] = (float)sl; f.o[c] = (float)of;
        for (int v = 0; v < 256; v++) { double d = pr[map[v]][c] - (sl * pr[v][c] + of); err += d * d; }
    }
    double rms = sqrt(err / 768); f.ok = rms < 0.045 && f.s[0] > -0.2 && f.s[1] > -0.2 && f.s[2] > -0.2 && f.s[0] < 1.6 && f.s[1] < 1.6 && f.s[2] < 1.6; return f;
}
struct GwGpu {
    bool init = false, fail = false; int w = 0, h = 0; UINT tblUp = 0; LONG epoch = 0; int badChecks = 0;
    ID3D11PixelShader *psC = nullptr, *psG = nullptr; ID3D11Buffer *cb = nullptr, *bA = nullptr, *bB = nullptr, *bC = nullptr, *bOff = nullptr, *bIdx = nullptr;
    ID3D11Texture2D *tBase = nullptr, *tAtlas = nullptr, *tTbl = nullptr, *tO1 = nullptr, *tO2 = nullptr, *tCell = nullptr, *tLut = nullptr, *tStage = nullptr;
    ID3D11Texture2D *tPal = nullptr, *tC1 = nullptr, *tWRgb = nullptr, *tPRgb = nullptr; ID3D11Buffer *bMS = nullptr, *bMO = nullptr, *bFS = nullptr, *bFO = nullptr;
    ID3D11ShaderResourceView *sPal = nullptr, *sC1 = nullptr, *sWRgb = nullptr, *sPRgb = nullptr, *sMS = nullptr, *sMO = nullptr, *sFS = nullptr, *sFO = nullptr; ID3D11RenderTargetView *rC1 = nullptr, *rWRgb = nullptr;
    UINT fitN = 0; unsigned palHash = 0, lutHash = 0; bool rgbValid = false;
    ID3D11ShaderResourceView *sBase = nullptr, *sAtlas = nullptr, *sTbl = nullptr, *sO1 = nullptr, *sCell = nullptr, *sLut = nullptr, *sA = nullptr, *sB = nullptr, *sC = nullptr, *sOff = nullptr, *sIdx = nullptr;
    ID3D11RenderTargetView *rO1 = nullptr, *rO2 = nullptr;
    long frames = 0, used = 0, cmpDiffFrames = 0; double lastLog = 0;
} g_gw;
static void GwRelease() {
    GwGpu& m = g_gw; IUnknown* rs[] = {m.psC, m.psG, m.cb, m.bA, m.bB, m.bC, m.bOff, m.bIdx, m.tBase, m.tAtlas, m.tTbl, m.tO1, m.tO2, m.tCell, m.tLut, m.tStage, m.sBase, m.sAtlas, m.sTbl, m.sO1, m.sCell, m.sLut, m.sA, m.sB, m.sC, m.sOff, m.sIdx, m.rO1, m.rO2, m.tPal, m.tC1, m.tWRgb, m.tPRgb, m.bMS, m.bMO, m.bFS, m.bFO, m.sPal, m.sC1, m.sWRgb, m.sPRgb, m.sMS, m.sMO, m.sFS, m.sFO, m.rC1, m.rWRgb};
    for (auto* r : rs) if (r) r->Release();
    bool f = m.fail; int bc = m.badChecks; m = GwGpu(); m.fail = f; m.badChecks = bc;
}
static bool GwInit(Gpu& g, int w, int h) {
    GwGpu& m = g_gw; if (m.fail) return false; if (m.init && m.w == w && m.h == h) return true;
    if (m.init) GwRelease();                                                                                           // resolution change: rebuild the targets
    const char* ents[2] = {"PSC", "PSG"}; ID3D11PixelShader** outs[2] = {&m.psC, &m.psG};
    for (int i = 0; i < 2; i++) { ID3DBlob *b = nullptr, *e = nullptr; HRESULT hr = D3DCompile(kGwHlsl, strlen(kGwHlsl), nullptr, nullptr, nullptr, ents[i], "ps_4_0", 0, 0, &b, &e);
        if (FAILED(hr)) { Log("gpuworld: shader %s failed: %s", ents[i], e ? (char*)e->GetBufferPointer() : "?"); m.fail = true; if (b) b->Release(); if (e) e->Release(); return false; }
        g.dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, outs[i]); b->Release(); if (e) e->Release(); }
    D3D11_TEXTURE2D_DESC td = {}; td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    auto mk = [&](int W, int H, DXGI_FORMAT f, UINT bind, ID3D11Texture2D** t, ID3D11ShaderResourceView** sv, DXGI_FORMAT svf) { td.Width = W; td.Height = H; td.Format = f; td.BindFlags = bind; if (FAILED(g.dev->CreateTexture2D(&td, nullptr, t))) return false;
        if (sv) { D3D11_SHADER_RESOURCE_VIEW_DESC d = {}; d.Format = svf; d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; d.Texture2D.MipLevels = 1; if (FAILED(g.dev->CreateShaderResourceView(*t, &d, sv))) return false; } return true; };
    bool ok = mk(w, h, DXGI_FORMAT_R8_UINT, D3D11_BIND_SHADER_RESOURCE, &m.tBase, &m.sBase, DXGI_FORMAT_R8_UINT)
        && mk(ATLAS_W, ATLAS_H, DXGI_FORMAT_R16_UINT, D3D11_BIND_SHADER_RESOURCE, &m.tAtlas, &m.sAtlas, DXGI_FORMAT_R16_UINT)
        && mk(256, 4096, DXGI_FORMAT_R8_UINT, D3D11_BIND_SHADER_RESOURCE, &m.tTbl, &m.sTbl, DXGI_FORMAT_R8_UINT)
        && mk(w, h, DXGI_FORMAT_R8_TYPELESS, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, &m.tO1, &m.sO1, DXGI_FORMAT_R8_UINT)
        && mk(w, h, DXGI_FORMAT_R8_UNORM, D3D11_BIND_RENDER_TARGET, &m.tO2, nullptr, DXGI_FORMAT_UNKNOWN)
        && mk(128, 128, DXGI_FORMAT_R32_UINT, D3D11_BIND_SHADER_RESOURCE, &m.tCell, &m.sCell, DXGI_FORMAT_R32_UINT)
        && mk(256, 128, DXGI_FORMAT_R8_UINT, D3D11_BIND_SHADER_RESOURCE, &m.tLut, &m.sLut, DXGI_FORMAT_R8_UINT)
        && mk(256, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, &m.tPal, &m.sPal, DXGI_FORMAT_R8G8B8A8_UNORM)
        && mk(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, &m.tC1, &m.sC1, DXGI_FORMAT_R8G8B8A8_UNORM)
        && mk(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, &m.tWRgb, &m.sWRgb, DXGI_FORMAT_R8G8B8A8_UNORM)
        && mk(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, &m.tPRgb, &m.sPRgb, DXGI_FORMAT_R8G8B8A8_UNORM);
    if (ok) { ok = SUCCEEDED(g.dev->CreateRenderTargetView(m.tC1, nullptr, &m.rC1)) && SUCCEEDED(g.dev->CreateRenderTargetView(m.tWRgb, nullptr, &m.rWRgb)); }
    if (ok) { D3D11_RENDER_TARGET_VIEW_DESC rd = {}; rd.Format = DXGI_FORMAT_R8_UNORM; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D; ok = SUCCEEDED(g.dev->CreateRenderTargetView(m.tO1, &rd, &m.rO1)) && SUCCEEDED(g.dev->CreateRenderTargetView(m.tO2, &rd, &m.rO2));
        td.Width = w; td.Height = h; td.Format = DXGI_FORMAT_R8_UNORM; td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; ok = ok && SUCCEEDED(g.dev->CreateTexture2D(&td, nullptr, &m.tStage)); }
    auto mkb = [&](UINT elems, UINT stride, DXGI_FORMAT f, ID3D11Buffer** b, ID3D11ShaderResourceView** sv) { D3D11_BUFFER_DESC bd = {}; bd.ByteWidth = elems * stride; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_SHADER_RESOURCE; if (FAILED(g.dev->CreateBuffer(&bd, nullptr, b))) return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC d = {}; d.Format = f; d.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; d.Buffer.FirstElement = 0; d.Buffer.NumElements = elems; return SUCCEEDED(g.dev->CreateShaderResourceView(*b, &d, sv)); };
    ok = ok && mkb(GW_MAXREC, 16, DXGI_FORMAT_R32G32B32A32_SINT, &m.bA, &m.sA) && mkb(GW_MAXREC, 16, DXGI_FORMAT_R32G32B32A32_SINT, &m.bB, &m.sB) && mkb(GW_MAXREC, 16, DXGI_FORMAT_R32G32B32A32_SINT, &m.bC, &m.sC)
        && mkb(20000, 4, DXGI_FORMAT_R32_UINT, &m.bOff, &m.sOff) && mkb(GW_MAXIDX, 4, DXGI_FORMAT_R32_UINT, &m.bIdx, &m.sIdx)
        && mkb(4096, 16, DXGI_FORMAT_R32G32B32A32_FLOAT, &m.bMS, &m.sMS) && mkb(4096, 16, DXGI_FORMAT_R32G32B32A32_FLOAT, &m.bMO, &m.sMO)
        && mkb(128, 16, DXGI_FORMAT_R32G32B32A32_FLOAT, &m.bFS, &m.sFS) && mkb(128, 16, DXGI_FORMAT_R32G32B32A32_FLOAT, &m.bFO, &m.sFO);
    D3D11_BUFFER_DESC cbd = {}; cbd.ByteWidth = 32; cbd.Usage = D3D11_USAGE_DEFAULT; cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; ok = ok && SUCCEEDED(g.dev->CreateBuffer(&cbd, nullptr, &m.cb));
    if (!ok) { Log("gpuworld: resource creation failed"); m.fail = true; return false; }
    m.init = true; m.w = w; m.h = h; Log("gpuworld: GPU world compositor ready (%dx%d, atlas %dx%d)", w, h, ATLAS_W, ATLAS_H); return true;
}
static void GwBoxAt(ID3D11Buffer* b, UINT off, const void* data, UINT bytes, Gpu& g) { if (!bytes) return; D3D11_BOX bx = {off, 0, 0, off + bytes, 1, 1}; g.ctx->UpdateSubresource(b, 0, &bx, data, 0, 0); }
static void GwBox(ID3D11Buffer* b, const void* data, UINT bytes, Gpu& g) { if (!bytes) return; D3D11_BOX bx = {0, 0, 0, bytes, 1, 1}; g.ctx->UpdateSubresource(b, 0, &bx, data, 0, 0); }
// returns true when g.tWorld now holds the GPU-composed world layer for snapshot sequence `seq`
static bool GwRun(Gpu& g, int w, int h, LONG seq, const Pal* pal) {
    std::shared_ptr<GpuWorldJob> job; EnterCriticalSection(&g_gwCs); job = g_jobPub; LeaveCriticalSection(&g_gwCs);
    { static long calls, noJob, badSeq, notOk, badSize, faultC; static double lastL; calls++; if (g_gwFault) faultC++; else if (!job) noJob++; else if (job->seq != seq) badSeq++; else if (!job->ok) notOk++; else if (job->W != w || job->H != h) badSize++;
      double nowL = Qpc(); if (nowL - lastL > 5000) { lastL = nowL; Log("gpuworld: %ld presenter checks | no job %ld, wrong sequence %ld, job refused %ld, wrong size %ld, fault %ld", calls, noJob, badSeq, notOk, badSize, faultC); calls = noJob = badSeq = notOk = badSize = faultC = 0; } }
    if (g_gwFault || !job || job->seq != seq || !job->ok || job->W != w || job->H != h) return false; if (!GwInit(g, w, h)) return false; GwGpu& m = g_gw; ID3D11DeviceContext* c = g.ctx;
    if (m.epoch != g_gwEpoch) { m.epoch = g_gwEpoch; m.tblUp = 0; }
    EnterCriticalSection(&g_gwCs);                                                                                   // atlas / tables written by the game thread
    if (g_atlasDirtyY1 >= g_atlasDirtyY0) { D3D11_BOX bx = {0, (UINT)g_atlasDirtyY0, 0, ATLAS_W, (UINT)g_atlasDirtyY1 + 1, 1}; c->UpdateSubresource(m.tAtlas, 0, &bx, g_atlasBits.data() + (size_t)g_atlasDirtyY0 * ATLAS_W, ATLAS_W * 2, 0); g_atlasDirtyY0 = 1 << 30; g_atlasDirtyY1 = -1; }
    if (g_tbl.size() > 4096) { LeaveCriticalSection(&g_gwCs); return false; }
    if (m.tblUp < g_tbl.size()) { D3D11_BOX bx = {0, m.tblUp, 0, 256, (UINT)g_tbl.size(), 1}; c->UpdateSubresource(m.tTbl, 0, &bx, g_tbl[m.tblUp].data(), 256, 0); m.tblUp = (UINT)g_tbl.size(); }
    float pr[256][3]; { unsigned h = 2166136261u; DWORD pe[256]; for (int i = 0; i < 256; i++) { PALETTEENTRY e = pal ? pal->e[i] : PALETTEENTRY{(BYTE)i, (BYTE)i, (BYTE)i, 0}; pr[i][0] = e.peRed / 255.0f; pr[i][1] = e.peGreen / 255.0f; pr[i][2] = e.peBlue / 255.0f; pe[i] = 0xFF000000 | (e.peBlue << 16) | (e.peGreen << 8) | e.peRed; h = (h ^ (pe[i] & 0xFFFFFF)) * 16777619u; }
        if (h != m.palHash) { m.palHash = h; m.fitN = 0; m.lutHash = 0; c->UpdateSubresource(m.tPal, 0, nullptr, pe, 1024, 0); } }
    if (m.fitN < g_tbl.size()) {                                                                                         // affine RGB model for every table that has not been fitted for this palette
        static std::vector<float> ms(4096 * 4), mo(4096 * 4); UINT n0 = m.fitN;
        for (UINT t = n0; t < g_tbl.size(); t++) { AffFit f = FitAffine(pr, g_tbl[t].data()); for (int k = 0; k < 3; k++) { ms[t * 4 + k] = f.s[k]; mo[t * 4 + k] = f.o[k]; } ms[t * 4 + 3] = f.ok ? 1.0f : 0.0f; mo[t * 4 + 3] = 0; }
        GwBoxAt(m.bMS, n0 * 16, ms.data() + n0 * 4, (UINT)(g_tbl.size() - n0) * 16, g); GwBoxAt(m.bMO, n0 * 16, mo.data() + n0 * 4, (UINT)(g_tbl.size() - n0) * 16, g); m.fitN = (UINT)g_tbl.size();
        if (n0 == 0) { /* full refit: buffers were written from index 0 */ } }
    LeaveCriticalSection(&g_gwCs);
    if (job->fogOn) { unsigned h = 2166136261u; for (size_t i = 0; i < 256 * 128; i += 4) h = (h ^ *(const unsigned*)(job->lut.data() + i)) * 16777619u; h ^= m.palHash;
        if (h != m.lutHash) { m.lutHash = h; float fs[128 * 4], fo[128 * 4]; for (int l = 0; l < 128; l++) { AffFit f = FitAffine(pr, job->lut.data() + l * 256); for (int k = 0; k < 3; k++) { fs[l * 4 + k] = f.s[k]; fo[l * 4 + k] = f.o[k]; } fs[l * 4 + 3] = f.ok ? 1.0f : 0.0f; fo[l * 4 + 3] = 0; }
            GwBox(m.bFS, fs, sizeof(fs), g); GwBox(m.bFO, fo, sizeof(fo), g); } }
    if (m.rgbValid) c->CopyResource(m.tPRgb, m.tWRgb);                                                                 // previous true-colour world layer (scroll interpolation)
    c->UpdateSubresource(m.tBase, 0, nullptr, job->base.data(), job->P, 0);
    std::vector<int> a(job->recs.size() * 4), b(job->recs.size() * 4), cc(job->recs.size() * 4);
    for (size_t i = 0; i < job->recs.size(); i++) { const GwRec& q = job->recs[i]; a[i * 4] = q.bx; a[i * 4 + 1] = q.by; a[i * 4 + 2] = q.w; a[i * 4 + 3] = q.h; b[i * 4] = q.ax; b[i * 4 + 1] = q.ay; cc[i * 4] = q.cx0; cc[i * 4 + 1] = q.cx1; cc[i * 4 + 2] = q.cy0; cc[i * 4 + 3] = q.cy1; }
    GwBox(m.bA, a.data(), (UINT)a.size() * 4, g); GwBox(m.bB, b.data(), (UINT)b.size() * 4, g); GwBox(m.bC, cc.data(), (UINT)cc.size() * 4, g);
    if (job->tileOff.size() > 20000) return false; GwBox(m.bOff, job->tileOff.data(), (UINT)job->tileOff.size() * 4, g); GwBox(m.bIdx, job->tileIdx.data(), (UINT)job->tileIdx.size() * 4, g);
    if (job->fogOn) { c->UpdateSubresource(m.tCell, 0, nullptr, job->cells.data(), 128 * 4, 0); c->UpdateSubresource(m.tLut, 0, nullptr, job->lut.data(), 256, 0); }
    unsigned cbv[8] = {(unsigned)job->tilesW, job->fogOn ? 1u : 0u, (unsigned)job->cols, (unsigned)job->rows, (unsigned)job->ox, (unsigned)job->oy, 0, 0}; c->UpdateSubresource(m.cb, 0, nullptr, cbv, 0, 0);
    ID3D11ShaderResourceView* nul[16] = {}; ID3D11RenderTargetView* nrt = nullptr; ID3D11RenderTargetView* nrt2[2] = {}; D3D11_VIEWPORT vp = {0, 0, (float)w, (float)h, 0, 1};
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); c->IASetInputLayout(nullptr); c->VSSetShader(g.vs, nullptr, 0); c->RSSetViewports(1, &vp); c->PSSetConstantBuffers(0, 1, &m.cb);
    ID3D11ShaderResourceView* s1[11] = {m.sBase, m.sAtlas, m.sTbl, m.sA, m.sB, m.sC, m.sOff, m.sIdx, m.sPal, m.sMS, m.sMO}; ID3D11RenderTargetView* r1[2] = {m.rO1, m.rC1};
    c->OMSetRenderTargets(2, r1, nullptr); c->PSSetShader(m.psC, nullptr, 0); c->PSSetShaderResources(0, 11, s1); c->Draw(3, 0);
    c->PSSetShaderResources(0, 11, nul); c->OMSetRenderTargets(2, nrt2, nullptr);
    ID3D11ShaderResourceView* s2[7] = {m.sO1, m.sCell, m.sLut, m.sPal, m.sFS, m.sFO, m.sC1}; ID3D11RenderTargetView* r2[2] = {m.rO2, m.rWRgb};
    c->OMSetRenderTargets(2, r2, nullptr); c->PSSetShader(m.psG, nullptr, 0); c->PSSetShaderResources(0, 7, s2); c->Draw(3, 0);
    c->PSSetShaderResources(0, 11, nul); c->OMSetRenderTargets(2, nrt2, nullptr);
    c->CopyResource(g.tWorld, m.tO2); m.rgbValid = true; m.frames++;
#ifdef REMASTER_DEV
    double now = Qpc(); if (m.frames <= 3 || now - m.lastLog > 5000) {                                                 // developer build: read back and compare with the game's own snapshot of the same frame
        m.lastLog = now; c->CopyResource(m.tStage, m.tO2); D3D11_MAPPED_SUBRESOURCE mp;
        if (SUCCEEDED(c->Map(m.tStage, 0, D3D11_MAP_READ, 0, &mp))) { long diff = 0, tot = (long)w * h; int fx = -1, fy = -1;
            for (int y = 0; y < h; y++) { const BYTE* x = (const BYTE*)mp.pData + (size_t)y * mp.RowPitch; const BYTE* r = g_world.buf + (size_t)y * g_world.pitch; if (memcmp(x, r, w) == 0) continue; for (int i = 0; i < w; i++) if (x[i] != r[i]) { if (!diff) { fx = i; fy = y; } diff++; } }
            c->Unmap(m.tStage, 0); Log("gpuworld: frame %ld: GPU world layer vs game snapshot: %ld of %ld px differ (%.4f%%), first at (%d,%d); %zu records", m.frames, diff, tot, 100.0 * diff / tot, fx, fy, job->recs.size()); }
    }
#endif
    return true;
}
#endif
