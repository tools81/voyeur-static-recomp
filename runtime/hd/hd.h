/* voyeur_hd: AI-upscaled video pack, per-plane capture and interpolation. */
#pragma once
#include <stdint.h>

#define HD_W 384
#define HD_H 240
#define HD_FB_W 768

/* Per field, written by video.c (HD build) while composing: each plane on
   its own with an opacity mask, the cursor on its own, and per-line
   composition parameters, so the renderer can rebuild the picture at high
   resolution with upscaled planes. */
typedef struct {
    float icf[2];         /* plane brightness 0..1 */
    float simple;         /* 1: plain "front over back over backdrop" (no mix, mattes, mosaic) */
    float b_front;        /* 1: plane B in front */
    float bd[3];          /* backdrop colour 0..1 */
    float natural[2];     /* plane is photographic (RL7 / DYUV) on this line */
    float pad[3];
} HdLine;                 /* 12 floats = 3 texels of RGBA32F */

typedef struct {
    uint32_t plane[2][HD_H * HD_FB_W];   /* host BGRA, alpha 255 = opaque */
    uint32_t overlay[HD_H * HD_FB_W];    /* cursor, alpha 255 where drawn */
    HdLine line[HD_H];
} HdField;
extern HdField g_hdf;

/* ---- pack (hd_pack.c) */
typedef struct {
    int img;              /* picture index, -1 = none */
    int dy;               /* picture row = display line + dy */
    int nok;
    uint8_t ok[HD_H];     /* display line verified against the picture's line hashes */
} HdMatch;

typedef struct {
    int ready;            /* decoded and available */
    int w, h;             /* hi-res size */
    const uint32_t *rgb;  /* w*h BGRA */
    const uint8_t *mask;  /* 384*240 alpha (NULL: opaque, use the live plane mask) */
    const uint8_t *flow;  /* 192*120*2 (dx, dy) toward the next picture, NULL if none */
} HdPic;

int  hd_pack_open(const char *dir);     /* 0 ok */
int  hd_pack_loaded(void);
int  hd_match(const uint64_t *line_hash, HdMatch *m);
int  hd_next(int img);                  /* next frame of the same clip, -1 if none */
int  hd_is_video(int img);
void hd_request(int img, int prio);     /* ask the loader thread for a picture */
int  hd_get(int img, HdPic *out);       /* 1 if ready; pins it until hd_release_all */
const char *hd_pack_info(void);
