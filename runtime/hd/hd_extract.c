/* voyeur_hd --hd-extract DIR [--hd-clip N]

   Decodes Voyeur's real-time video file (RTVA.rtf) straight from the disc
   and writes, per clip, the native frames and background for the AI
   upscaling pipeline (tools/hd_pack.py):

     DIR/clip_NN/clip.txt     name, fps, frame count; "f <i> <hash>" per frame,
                              "bg <hash>" for the background still
     DIR/clip_NN/f_NNNNN.tga  384x240 RGBA frame (alpha 0 = transparent key)
     DIR/clip_NN/bg.tga       384x240 DYUV background still (if present)

   Clip layout (channel 0; audio on channel 1 is skipped):
     header data sectors: +0 number of header sectors, +0x24 frame count,
       +0x26 frame rate, +0x6A 128 RGB colours (studio range), +0x36A one
       byte per frame = video sectors in that frame;
     DYUV still sectors (coding 05): a 384x240 background;
     RL7 frames (coding 0x90): 240 run-length lines, sector aligned.
   The hashes match what the runtime computes for a plane showing that
   picture (fnv64 over the coded bytes of its 240 lines, see video.c). */
#define EMU_HOST_SIDE
#include "runtime.h"
#include <stdlib.h>
#include <string.h>
#include <direct.h>

#define W 384
#define H 240
#define RAWSZ 2352

static int to_host(int v) { v = (v - 16) * 255 / 219; return v < 0 ? 0 : v > 255 ? 255 : v; }

static void write_tga(const char *path, const uint32_t *argb) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint8_t h[18] = {0};
    h[2] = 2;
    h[12] = W & 255; h[13] = W >> 8; h[14] = H & 255; h[15] = H >> 8;
    h[16] = 32; h[17] = 0x28;           /* top-left origin, 8 alpha bits */
    fwrite(h, 1, 18, f);
    fwrite(argb, 4, (size_t)W * H, f);  /* little-endian ARGB = BGRA bytes */
    fclose(f);
}

static const int dyuv_q[16] = { 0, 1, 4, 9, 16, 27, 44, 79, 128, 177, 212, 229, 240, 247, 252, 255 };
static int clampi_(int v, int a, int b) { return v < a ? a : v > b ? b : v; }
static uint32_t yuv_rgb(int y, int u, int v) {
    int b = y + (u - 128) * 1733 / 1000;
    int r = y + (v - 128) * 1371 / 1000;
    int g = (y * 1000 - 299 * r - 114 * b) / 587;      /* as video.c */
    r = to_host(clampi_(r, 0, 255)); g = to_host(clampi_(g, 0, 255)); b = to_host(clampi_(b, 0, 255));
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

typedef struct {
    uint32_t hdr_lsn;
    uint8_t hdr[2048 * 8];
    int nhdr;
    uint32_t *vs; int nvs, cvs;      /* RL7 video sectors */
    uint32_t dy[64]; int ndy;        /* DYUV still sectors */
} Clip;

static void payload(uint32_t lsn, uint8_t *out, int *n) {
    uint8_t raw[RAWSZ];
    disc_read_raw(lsn, raw);
    int f2 = raw[18] & 0x20;
    *n = f2 ? 2324 : 2048;
    memcpy(out, raw + 24, (size_t)*n);
}

int hd_extract_noimages;   /* --hd-hashes: clip.txt only */

int hd_extract(const char *dir, int only_clip) {
    const DiscFile *df = disc_find("RTVA.rtf");
    if (!df) { LOG("RTVA.rtf not found"); return 1; }
    uint32_t nsect = df->size / 2048;
    Clip *clips = (Clip *)calloc(128, sizeof(Clip));
    int nclips = 0;
    Clip *cur = NULL;
    uint8_t raw[RAWSZ];
    for (uint32_t i = 0; i < nsect; i++) {
        disc_read_raw(df->lsn + i, raw);
        int ch = raw[17], sub = raw[18], cod = raw[19];
        if (ch != 0) continue;
        if ((sub & 0x08) && !(sub & 0x20)) {
            if (!cur || cur->nvs || cur->ndy) {
                if (nclips >= 128) break;
                cur = &clips[nclips++];
                cur->hdr_lsn = df->lsn + i;
            }
            if (cur->nhdr < 8) memcpy(cur->hdr + 2048 * cur->nhdr++, raw + 24, 2048);
        } else if (cur && (sub & 0x02)) {
            if (cod == 0x05 && cur->ndy < 64) cur->dy[cur->ndy++] = df->lsn + i;
            else if (cod == 0x90) {
                if (cur->nvs == cur->cvs) {
                    cur->cvs = cur->cvs ? cur->cvs * 2 : 1024;
                    cur->vs = (uint32_t *)realloc(cur->vs, cur->cvs * sizeof(uint32_t));
                }
                cur->vs[cur->nvs++] = df->lsn + i;
            }
        }
    }
    LOG("hd-extract: %d clips", nclips);
    _mkdir(dir);
    uint32_t *img = (uint32_t *)malloc((size_t)W * H * 4);
    uint8_t *data = (uint8_t *)malloc(2324 * 256);
    for (int c = 0; c < nclips; c++) {
        if (only_clip >= 0 && c != only_clip) continue;
        Clip *C = &clips[c];
        const uint8_t *h = C->hdr;
        int frames = (h[0x24] << 8) | h[0x25], fps = (h[0x26] << 8) | h[0x27];
        char name[17] = {0};
        memcpy(name, h + 0x12, 16);
        uint32_t clut[128];
        int key[128];
        for (int k = 0; k < 128; k++) {
            const uint8_t *e = h + 0x6A + k * 3;
            key[k] = e[0] == 0 && e[1] == 0xFF && e[2] == 0;      /* green: the transparent colour key */
            clut[k] = 0xFF000000u | ((uint32_t)to_host(e[0]) << 16) | ((uint32_t)to_host(e[1]) << 8) | (uint32_t)to_host(e[2]);
        }
        char cd[600], p[700];
        snprintf(cd, sizeof cd, "%s/clip_%02d", dir, c);
        _mkdir(cd);
        snprintf(p, sizeof p, "%s/clip.txt", cd);
        FILE *tf = fopen(p, "w");
        if (!tf) continue;
        fprintf(tf, "name %s\nfps %d\nframes %d\nlsn %u\n", name, fps, frames, C->hdr_lsn);
        /* background still */
        if (C->ndy * 2324 >= W * H) {
            int n = 0, k = 0;
            for (int s = 0; s < C->ndy; s++) { payload(C->dy[s], data + k, &n); k += n; }
            uint64_t hh = FNV64_INIT, lh[H];
            const uint8_t *d = data;
            for (int y = 0; y < H; y++) {
                int yy = 128, u = 128, v = 128, pu = 128, pv = 128;   /* the game starts each line at 80 80 80 */
                hh = fnv64(hh, d, W);
                lh[y] = fnv64(FNV64_INIT, d, W);
                for (int x = 0; x < W; x += 2) {
                    uint8_t b1 = *d++, b2 = *d++;
                    int nu = (u + dyuv_q[b1 >> 4]) & 255, nv = (v + dyuv_q[b2 >> 4]) & 255;
                    int y0 = (yy + dyuv_q[b1 & 15]) & 255, y1 = (y0 + dyuv_q[b2 & 15]) & 255;
                    img[y * W + x] = yuv_rgb(y0, (pu + nu) / 2, (pv + nv) / 2);
                    img[y * W + x + 1] = yuv_rgb(y1, nu, nv);
                    pu = nu; pv = nv; u = nu; v = nv; yy = y1;
                }
            }
            snprintf(p, sizeof p, "%s/bg.tga", cd);
            if (!hd_extract_noimages) write_tga(p, img);
            fprintf(tf, "bg %016llx\n", (unsigned long long)hh);
            snprintf(p, sizeof p, "%s/bglines.bin", cd);
            FILE *bf = fopen(p, "wb");
            if (bf) { fwrite(lh, 8, H, bf); fclose(bf); }
        }
        /* frames; lines.bin: 240 line hashes per frame */
        snprintf(p, sizeof p, "%s/lines.bin", cd);
        FILE *lf = fopen(p, "wb");
        int vk = 0;
        for (int fi = 0; fi < frames && fi < 0x2000; fi++) {
            int ns = h[0x36A + fi];
            if (ns <= 0 || ns > 256 || vk + ns > C->nvs) break;
            int len = 0, n = 0;
            for (int s = 0; s < ns; s++) { payload(C->vs[vk + s], data + len, &n); len += n; }
            vk += ns;
            int pos = 0;
            uint64_t hh = FNV64_INIT, lh[H];
            for (int y = 0; y < H; y++) {
                int x = 0, p0 = pos;
                while (x < W && pos < len) {
                    uint8_t b = data[pos++];
                    int run = 1;
                    if (b & 0x80) { run = pos < len ? data[pos++] : 1; if (run == 0) run = W - x; }
                    for (int k = 0; k < run && x < W; k++, x++)
                        img[y * W + x] = key[b & 0x7F] ? 0 : clut[b & 0x7F];
                }
                for (; x < W; x++) img[y * W + x] = 0;
                hh = fnv64(hh, data + p0, (size_t)(pos - p0));
                lh[y] = fnv64(FNV64_INIT, data + p0, (size_t)(pos - p0));
            }
            if (lf) fwrite(lh, 8, H, lf);
            snprintf(p, sizeof p, "%s/f_%05d.tga", cd, fi);
            if (!hd_extract_noimages) write_tga(p, img);
            fprintf(tf, "f %d %016llx\n", fi, (unsigned long long)hh);
        }
        fclose(tf);
        if (lf) fclose(lf);
        LOG("clip %d %s: %d frames at %d fps", c, name, frames, fps);
    }
    free(img); free(data);
    return 0;
}
