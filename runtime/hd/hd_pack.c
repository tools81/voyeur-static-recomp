/* HD video pack: index, picture matching and a background loader.

   hd_pack/index.bin (tools/hd_pack.py):
     "VHD1", u32 version, scale, nclips, nimages, nkeys
     clips[nclips]   u32 clip number, fps, frames, first picture
     images[nimages] u32 clip index, i32 frame (-1 = background still), u32 flags
                     (1 picture, 2 mask, 4 flow to the next frame, 8 video frame)
     keys[nkeys]     u64 line hash, u32 picture, u32 row   (sorted by hash)
   hd_pack/img/NN/  fNNNNN.jpg (upscaled), fNNNNN.m.png (native alpha mask),
                    fNNNNN.v.png (flow to the next frame), bg.jpg,
                    lines.bin / bglines.bin (240 line hashes per picture)

   A plane on screen is matched by looking up the hashes of its coded lines
   among the keys (line hashes unique on the whole disc), voting for a
   (picture, vertical offset), then checking every line against the
   picture's own line hashes. */
#define EMU_HOST_SIDE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "hd.h"

typedef struct { uint32_t clip_no, fps, frames, first; } PClip;
typedef struct { uint32_t clip; int32_t frame; uint32_t flags; } PImg;
#pragma pack(push, 4)
typedef struct { uint64_t hash; uint32_t img, row; } PKey;
#pragma pack(pop)

static PClip *clips;
static PImg *imgs;
static PKey *keys;
static uint32_t nclips, nimgs, nkeys, pscale;
static char packdir[600];
static uint64_t **clip_lines;     /* per clip: (frames + bg) * 240, loaded on first use */
static char info[256];

/* ---------------------------------------------------------------- index */
int hd_pack_open(const char *dir) {
    snprintf(packdir, sizeof packdir, "%s", dir);
    char p[700];
    snprintf(p, sizeof p, "%s/index.bin", dir);
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    char magic[4];
    uint32_t hdr[5];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "VHD1", 4) || fread(hdr, 4, 5, f) != 5 || hdr[0] != 1) {
        fclose(f);
        LOG("HD pack: %s is not a version 1 index", p);
        return -1;
    }
    pscale = hdr[1]; nclips = hdr[2]; nimgs = hdr[3]; nkeys = hdr[4];
    clips = (PClip *)calloc(nclips + 1, sizeof(PClip));
    imgs = (PImg *)calloc(nimgs + 1, sizeof(PImg));
    keys = (PKey *)calloc(nkeys + 1, sizeof(PKey));
    clip_lines = (uint64_t **)calloc(nclips + 1, sizeof(uint64_t *));
    size_t ok = fread(clips, sizeof(PClip), nclips, f) == nclips &&
                fread(imgs, sizeof(PImg), nimgs, f) == nimgs &&
                fread(keys, sizeof(PKey), nkeys, f) == nkeys;
    fclose(f);
    if (!ok) { LOG("HD pack: truncated index"); nimgs = 0; return -1; }
    snprintf(info, sizeof info, "%u clips, %u pictures, %ux", nclips, nimgs, pscale);
    LOG("HD pack: %s (%s)", dir, info);
    return 0;
}

int hd_pack_loaded(void) { return nimgs > 0; }
const char *hd_pack_info(void) { return info; }

static const uint64_t *pic_lines(int img) {
    const PImg *I = &imgs[img];
    uint32_t c = I->clip;
    if (!clip_lines[c]) {
        const PClip *C = &clips[c];
        size_t n = (size_t)(C->frames + 1) * HD_H;
        uint64_t *l = (uint64_t *)calloc(n, 8);
        char p[700];
        snprintf(p, sizeof p, "%s/img/%02u/lines.bin", packdir, C->clip_no);
        FILE *f = fopen(p, "rb");
        if (f) { fread(l, 8, (size_t)C->frames * HD_H, f); fclose(f); }
        snprintf(p, sizeof p, "%s/img/%02u/bglines.bin", packdir, C->clip_no);
        f = fopen(p, "rb");
        if (f) { fread(l + (size_t)C->frames * HD_H, 8, HD_H, f); fclose(f); }
        clip_lines[c] = l;
    }
    int row0 = I->frame < 0 ? (int)clips[c].frames : I->frame;
    return clip_lines[c] + (size_t)row0 * HD_H;
}

int hd_next(int img) {
    if (img < 0 || img + 1 >= (int)nimgs) return -1;
    const PImg *a = &imgs[img], *b = &imgs[img + 1];
    if (a->frame < 0 || b->clip != a->clip || b->frame != a->frame + 1) return -1;
    return img + 1;
}

int hd_is_video(int img) { return img >= 0 && img < (int)nimgs && (imgs[img].flags & 8); }

static int key_lo(uint64_t h) {
    int lo = 0, hi = (int)nkeys;
    while (lo < hi) {
        int m = (lo + hi) / 2;
        if (keys[m].hash < h) lo = m + 1; else hi = m;
    }
    return lo;
}

int hd_match(const uint64_t *lh, HdMatch *m) {
    m->img = -1; m->dy = 0; m->nok = 0;
    if (!nimgs) return 0;
    int cand_img[8], cand_dy[8], cand_n[8], nc = 0;
    for (int y = 0; y < HD_H; y++) {
        if (!lh[y]) continue;
        for (int k = key_lo(lh[y]); k < (int)nkeys && keys[k].hash == lh[y]; k++) {
            int img = (int)keys[k].img, dy = (int)keys[k].row - y, j;
            for (j = 0; j < nc && !(cand_img[j] == img && cand_dy[j] == dy); j++) {}
            if (j < nc) cand_n[j]++;
            else if (nc < 8) { cand_img[nc] = img; cand_dy[nc] = dy; cand_n[nc] = 1; nc++; }
        }
    }
    int best = -1;
    for (int j = 0; j < nc; j++) if (best < 0 || cand_n[j] > cand_n[best]) best = j;
    if (best < 0) return 0;
    const uint64_t *pl = pic_lines(cand_img[best]);
    int dy = cand_dy[best], nok = 0;
    for (int y = 0; y < HD_H; y++) {
        int r = y + dy;
        m->ok[y] = lh[y] && r >= 0 && r < HD_H && pl[r] == lh[y];
        nok += m->ok[y];
    }
    if (nok < 4) return 0;
    m->img = cand_img[best]; m->dy = dy; m->nok = nok;
    return 1;
}

/* ---------------------------------------------------------------- loader */
#define CACHE_N 48
enum { E_FREE, E_QUEUED, E_LOADING, E_READY, E_FAILED };
typedef struct {
    int img, state, prio;
    uint64_t used;
    int w, h;
    uint32_t *rgb;
    uint8_t *mask, *flow;
} Ent;
static Ent cache[CACHE_N];
static CRITICAL_SECTION lock;
static HANDLE wake;
static uint64_t tick;
static int started;

static int wic_load(IWICImagingFactory *fac, const char *path, REFWICPixelFormatGUID fmt, int bpp,
                    int *w, int *h, uint8_t **out) {
    wchar_t wp[700];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 700);
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *fr = NULL;
    IWICFormatConverter *cv = NULL;
    int ok = 0;
    if (SUCCEEDED(IWICImagingFactory_CreateDecoderFromFilename(fac, wp, NULL, GENERIC_READ,
                                                               WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &fr)) &&
        SUCCEEDED(IWICImagingFactory_CreateFormatConverter(fac, &cv)) &&
        SUCCEEDED(IWICFormatConverter_Initialize(cv, (IWICBitmapSource *)fr, fmt, WICBitmapDitherTypeNone, NULL, 0,
                                                 WICBitmapPaletteTypeCustom))) {
        UINT ww, hh;
        IWICBitmapFrameDecode_GetSize(fr, &ww, &hh);
        uint8_t *buf = (uint8_t *)malloc((size_t)ww * hh * bpp);
        if (buf && SUCCEEDED(IWICFormatConverter_CopyPixels(cv, NULL, ww * bpp, ww * hh * bpp, buf))) {
            *w = (int)ww; *h = (int)hh; *out = buf; ok = 1;
        } else free(buf);
    }
    if (cv) IWICFormatConverter_Release(cv);
    if (fr) IWICBitmapFrameDecode_Release(fr);
    if (dec) IWICBitmapDecoder_Release(dec);
    return ok;
}

static void load_ent(IWICImagingFactory *fac, Ent *e) {
    const PImg *I = &imgs[e->img];
    const PClip *C = &clips[I->clip];
    char base[700], p[760];
    if (I->frame < 0) snprintf(base, sizeof base, "%s/img/%02u/bg", packdir, C->clip_no);
    else snprintf(base, sizeof base, "%s/img/%02u/f%05d", packdir, C->clip_no, I->frame);
    int w, h;
    uint8_t *rgb = NULL, *mask = NULL, *fl = NULL;
    snprintf(p, sizeof p, "%s.jpg", base);
    if (!wic_load(fac, p, &GUID_WICPixelFormat32bppBGRA, 4, &w, &h, &rgb)) { e->state = E_FAILED; return; }
    if (I->flags & 2) {
        int mw, mh;
        snprintf(p, sizeof p, "%s.m.png", base);
        if (!wic_load(fac, p, &GUID_WICPixelFormat8bppGray, 1, &mw, &mh, &mask) || mw != HD_W || mh != HD_H) {
            free(mask); mask = NULL;
        }
    }
    if (I->flags & 4) {
        int fw, fh;
        uint8_t *bgr = NULL;
        snprintf(p, sizeof p, "%s.v.png", base);
        if (wic_load(fac, p, &GUID_WICPixelFormat24bppBGR, 3, &fw, &fh, &bgr) && fw == HD_W / 2 && fh == HD_H / 2) {
            fl = (uint8_t *)malloc((size_t)fw * fh * 2);
            for (int i = 0; i < fw * fh; i++) { fl[i * 2] = bgr[i * 3 + 2]; fl[i * 2 + 1] = bgr[i * 3 + 1]; }
        }
        free(bgr);
    }
    EnterCriticalSection(&lock);
    e->rgb = (uint32_t *)rgb; e->mask = mask; e->flow = fl; e->w = w; e->h = h;
    e->state = E_READY;
    LeaveCriticalSection(&lock);
}

static DWORD WINAPI loader(LPVOID arg) {
    (void)arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IWICImagingFactory *fac = NULL;
    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory,
                                (void **)&fac))) {
        LOG("HD pack: WIC unavailable");
        return 0;
    }
    for (;;) {
        Ent *job = NULL;
        EnterCriticalSection(&lock);
        for (int i = 0; i < CACHE_N; i++) {
            Ent *e = &cache[i];
            if (e->state != E_QUEUED) continue;
            if (!job || e->prio < job->prio || (e->prio == job->prio && e->used > job->used)) job = e;
        }
        if (job) job->state = E_LOADING;
        LeaveCriticalSection(&lock);
        if (!job) { WaitForSingleObject(wake, 100); continue; }
        load_ent(fac, job);
    }
}

static void start_loader(void) {
    if (started) return;
    started = 1;
    InitializeCriticalSection(&lock);
    wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    for (int i = 0; i < CACHE_N; i++) cache[i].img = -1;
    CreateThread(NULL, 0, loader, NULL, 0, NULL);
}

void hd_request(int img, int prio) {
    if (img < 0 || img >= (int)nimgs) return;
    start_loader();
    EnterCriticalSection(&lock);
    tick++;
    Ent *hit = NULL, *victim = NULL;
    for (int i = 0; i < CACHE_N; i++) {
        Ent *e = &cache[i];
        if (e->img == img) { hit = e; break; }
        if (e->state == E_LOADING) continue;
        if (!victim || e->state == E_FREE || (victim->state != E_FREE && e->used < victim->used)) victim = e;
    }
    if (hit) {
        hit->used = tick;
        if (hit->state == E_QUEUED && prio < hit->prio) hit->prio = prio;
    } else if (victim) {
        free(victim->rgb); free(victim->mask); free(victim->flow);
        victim->rgb = NULL; victim->mask = victim->flow = NULL;
        victim->img = img; victim->state = E_QUEUED; victim->prio = prio; victim->used = tick;
        SetEvent(wake);
    }
    LeaveCriticalSection(&lock);
}

int hd_get(int img, HdPic *out) {
    memset(out, 0, sizeof *out);
    if (img < 0 || !started) return 0;
    int r = 0;
    EnterCriticalSection(&lock);
    for (int i = 0; i < CACHE_N; i++) {
        Ent *e = &cache[i];
        if (e->img != img || e->state != E_READY) continue;
        e->used = ++tick;
        out->ready = 1; out->w = e->w; out->h = e->h;
        out->rgb = e->rgb; out->mask = e->mask; out->flow = e->flow;
        r = 1;
        break;
    }
    LeaveCriticalSection(&lock);
    return r;
}
