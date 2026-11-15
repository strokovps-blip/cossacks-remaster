// ---------------------------------------------------------------- diagnostics: PNG dumps and tick/work timing
static unsigned g_crcTab[256]; static bool g_crcInit;
static unsigned Crc(const u8* d, size_t n, unsigned c = 0xFFFFFFFFu) {
    if (!g_crcInit) { for (unsigned i = 0; i < 256; i++) { unsigned v = i; for (int k = 0; k < 8; k++) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1; g_crcTab[i] = v; } g_crcInit = true; }
    for (size_t i = 0; i < n; i++) c = g_crcTab[(c ^ d[i]) & 0xFF] ^ (c >> 8); return c;
}
static void PutBE(FILE* f, unsigned v) { u8 b[4] = {(u8)(v >> 24), (u8)(v >> 16), (u8)(v >> 8), (u8)v}; fwrite(b, 1, 4, f); }
static void PngChunk(FILE* f, const char* type, const u8* data, size_t n) {
    PutBE(f, (unsigned)n); fwrite(type, 1, 4, f); if (n) fwrite(data, 1, n, f);
    unsigned c = Crc((const u8*)type, 4); if (n) c = Crc(data, n, c); PutBE(f, c ^ 0xFFFFFFFFu);
}
static void WritePng(const char* path, int w, int h, const u8* rgb) {              // RGB 8-bit, stored (uncompressed) deflate
    FILE* f = fopen(path, "wb"); if (!f) return; const u8 sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10}; fwrite(sig, 1, 8, f);
    u8 ih[13] = {(u8)(w >> 24), (u8)(w >> 16), (u8)(w >> 8), (u8)w, (u8)(h >> 24), (u8)(h >> 16), (u8)(h >> 8), (u8)h, 8, 2, 0, 0, 0}; PngChunk(f, "IHDR", ih, 13);
    size_t rawN = (size_t)h * (w * 3 + 1); u8* raw = (u8*)malloc(rawN);
    for (int y = 0; y < h; y++) { raw[(size_t)y * (w * 3 + 1)] = 0; memcpy(raw + (size_t)y * (w * 3 + 1) + 1, rgb + (size_t)y * w * 3, (size_t)w * 3); }
    size_t blocks = (rawN + 65534) / 65535, zN = 2 + rawN + blocks * 5 + 4; u8* z = (u8*)malloc(zN); size_t o = 0; z[o++] = 0x78; z[o++] = 0x01;
    unsigned a = 1, b = 0; for (size_t i = 0; i < rawN; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    for (size_t i = 0; i < rawN; i += 65535) { size_t n = rawN - i < 65535 ? rawN - i : 65535; z[o++] = (i + n >= rawN) ? 1 : 0; z[o++] = (u8)n; z[o++] = (u8)(n >> 8); z[o++] = (u8)~n; z[o++] = (u8)(~n >> 8); memcpy(z + o, raw + i, n); o += n; }
    unsigned ad = (b << 16) | a; z[o++] = (u8)(ad >> 24); z[o++] = (u8)(ad >> 16); z[o++] = (u8)(ad >> 8); z[o++] = (u8)ad;
    PngChunk(f, "IDAT", z, o); PngChunk(f, "IEND", nullptr, 0); fclose(f); free(raw); free(z);
}
static void DumpIndexed(const char* path, const u8* idx, int pitch, int w, int h, int x0, int y0, int cw, int ch, const Pal* pal, int step) {
    int ow = cw / step, oh = ch / step; u8* rgb = (u8*)malloc((size_t)ow * oh * 3);
    for (int y = 0; y < oh; y++) for (int x = 0; x < ow; x++) { u8 i = idx[(size_t)(y0 + y * step) * pitch + x0 + x * step]; PALETTEENTRY e = pal ? pal->e[i] : PALETTEENTRY{i, i, i, 0}; u8* d = rgb + ((size_t)y * ow + x) * 3; d[0] = e.peRed; d[1] = e.peGreen; d[2] = e.peBlue; }
    WritePng(path, ow, oh, rgb); free(rgb);
}
// tick / work timing (filled by the game-thread hook and the presenter, printed every 2 s)
struct CamRec { double t; int x, y; }; static CamRec g_camRing[256]; static volatile LONG g_camN;
static double g_lastPumpExit; static volatile double g_lastFrameMs, g_lastPresentMs; static volatile LONG g_workOver40, g_workTicks; static double g_workMax;
