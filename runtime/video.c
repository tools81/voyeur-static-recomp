/* UCM video driver HLE + MCD212-style display renderer. */
#include "recomp.h"
#include "runtime.h"
#include <stdlib.h>
#include <string.h>
#ifdef RECOMP_HD
#include "hd/hd.h"
#endif
#include "emu_state.h"   /* machine state: must follow the other includes */

#define E_UNKSVC 0xD0
#define E_UNID   0xBC
#define E_IDFULL 0xBD

/* The display is rendered at double horizontal resolution (768) so that
   CLUT4/RL3 double-resolution planes fit; normal resolution pixels are
   written twice. */
#define SCREEN_LINES 240
#define SCREEN_W     768
int video_w = SCREEN_W, video_h = SCREEN_LINES;
uint32_t *video_fb;

/* ---------------------------------------------------------------- tables */
#define MAXFCT 32
#define MAXLCT 64
#define LCT_COLS 8

typedef struct {
    int used, plane, len, res;
    uint32_t *ins;
    int lct, lct_line;          /* FLnk */
} FCT;

typedef struct {
    int used, plane, lines, res;    /* lines = physical lines */
    uint32_t *ins;                  /* lines * LCT_COLS */
} LCT;

static FCT fcts[MAXFCT];
static LCT lcts[MAXLCT];
static int exec_fct[2];             /* active FCT id per plane (0 = none) */
static int ssig_sig, ssig_count = -1, ssig_pid = 1;
static int interlace;

#define LINK_OP 0x20000000u

static int lct_phys_line(LCT *l, int ucm_line) {
    return l->res ? ucm_line : ucm_line / 2;
}

/* ---------------------------------------------------------------- state */
typedef struct {
    uint32_t vsr;           /* current pixel fetch address */
    int vsr_set;
    uint32_t ddr;           /* display parameters ($78) */
    int icf;
    int mosaic, mosaic_en;
    uint32_t transp_color, mask_color;
    uint32_t dyuv_abs;
    int clut_bank;
} Plane;

static struct {
    uint32_t clut[256];
    uint32_t icm;           /* image coding methods ($C0) */
    uint32_t tcr;           /* transparency control ($C1) */
    uint32_t porder;        /* plane order ($C2) */
    uint32_t backdrop;
    uint32_t matte[8];
    Plane p[2];
    int interrupts;         /* $60 hits this field */
} vs;

/* graphics cursor (16x16, 1 bpp) */
static struct {
    int show, res;
    int hx, hy;             /* hit point position (high-res coords) */
    int hotx, hoty, w, h;   /* hit point within pattern, pattern size */
    int ox, oy;             /* cursor origin */
    uint32_t color;
    uint32_t blink;
    uint16_t ptn[32];
} gc;
static unsigned field_count;

/* ---------------------------------------------------------------- UCM */
static int alloc_id(int isfct) {
    if (isfct) { for (int i = 1; i < MAXFCT; i++) if (!fcts[i].used) return i; }
    else { for (int i = 1; i < MAXLCT; i++) if (!lcts[i].used) return i; }
    return -1;
}

static void dc_write_lct(int id, int line, int col, uint32_t ins) {
    if (id <= 0 || id >= MAXLCT || !lcts[id].used) return;
    LCT *l = &lcts[id];
    if (line < 0 || line >= l->lines || col < 0 || col >= LCT_COLS) return;
    l->ins[line * LCT_COLS + col] = ins;
}

int ucm_setstat(uint32_t code) {
    uint32_t fn = D(2) & 0xFFFF;
    int16_t d3 = (int16_t)D(3), d4 = (int16_t)D(4), d5 = (int16_t)D(5), d6 = (int16_t)D(6), d7 = (int16_t)D(7);
    if (code == 0x56) { /* SS_DC */
        switch (fn) {
        case 0x00: { /* DC_CrFCT plane, length, res */
            int id = alloc_id(1);
            if (id < 0) return E_IDFULL;
            FCT *f = &fcts[id];
            memset(f, 0, sizeof *f);
            f->used = 1; f->plane = d3 & 1; f->len = d4; f->res = D(5) & 0xFF;
            f->ins = (uint32_t *)emu_alloc(((size_t)f->len + 1) * 4);
            for (int i = 0; i < f->len; i++) f->ins[i] = 0x10000000;
            D(0) = (uint32_t)id;
            TRACE("DC_CrFCT plane %d len %d -> %d", f->plane, f->len, id);
            return 0;
        }
        case 0x01: case 0x02: { /* DC_RdFCT / DC_WrFCT id, start, count, a0 */
            if (d3 <= 0 || d3 >= MAXFCT || !fcts[d3].used) return E_UNID;
            FCT *f = &fcts[d3];
            for (int i = 0; i < d5; i++) {
                int k = d4 + i;
                if (k < 0 || k >= f->len) continue;
                if (fn == 2) f->ins[k] = RL(A(0) + i * 4);
                else WL(A(0) + i * 4, f->ins[k]);
            }
            return 0;
        }
        case 0x03: /* DC_RdFI */
            if (d3 <= 0 || d3 >= MAXFCT || !fcts[d3].used) return E_UNID;
            D(0) = (d4 >= 0 && d4 < fcts[d3].len) ? fcts[d3].ins[d4] : 0;
            return 0;
        case 0x04: /* DC_WrFI id, index, instr */
            if (d3 <= 0 || d3 >= MAXFCT || !fcts[d3].used) return E_UNID;
            if (d4 >= 0 && d4 < fcts[d3].len) fcts[d3].ins[d4] = D(5);
            return 0;
        case 0x05: /* DC_DlFCT */
            if (d3 > 0 && d3 < MAXFCT && fcts[d3].used) { emu_free(fcts[d3].ins); fcts[d3].used = 0; }
            return 0;
        case 0x06: { /* DC_CrLCT plane, lines (UCM), res */
            int id = alloc_id(0);
            if (id < 0) return E_IDFULL;
            LCT *l = &lcts[id];
            memset(l, 0, sizeof *l);
            l->used = 1; l->plane = d3 & 1; l->res = D(5) & 0xFF;
            l->lines = l->res ? d4 : (d4 + 1) / 2;
            l->ins = (uint32_t *)emu_alloc((size_t)l->lines * LCT_COLS * 4);
            for (int i = 0; i < l->lines * LCT_COLS; i++) l->ins[i] = 0x10000000;
            D(0) = (uint32_t)id;
            TRACE("DC_CrLCT plane %d lines %d -> %d", l->plane, l->lines, id);
            return 0;
        }
        case 0x07: case 0x08: case 0x1B: case 0x1C: { /* Rd/WrLCT, PRd/PWrLCT */
            if (d3 <= 0 || d3 >= MAXLCT || !lcts[d3].used) return E_UNID;
            LCT *l = &lcts[d3];
            int phys = (fn >= 0x1B);
            int wr = (fn == 0x08 || fn == 0x1C);
            int n = 0;
            if (log_level >= 2 && wr)
                TRACE("DC_%sWrLCT field %u lct %d line %d col %d lines %d cols %d a0 %06x: %08x %08x %08x %08x %08x %08x",
                      phys ? "P" : "", field_count, d3, d4, d5, d6, d7, A(0),
                      RL(A(0)), RL(A(0) + 4), RL(A(0) + 8), RL(A(0) + 12), RL(A(0) + 16), RL(A(0) + 20));
            for (int y = 0; y < d6; y++) {
                int line = phys ? d4 + y : lct_phys_line(l, d4 + y);
                for (int x = 0; x < d7; x++) {
                    uint32_t a = A(0) + (uint32_t)(y * d7 + x) * 4;
                    int col = d5 + x;
                    if (line < 0 || line >= l->lines || col >= LCT_COLS) continue;
                    if (wr) l->ins[line * LCT_COLS + col] = RL(a);
                    else WL(a, l->ins[line * LCT_COLS + col]);
                    n++;
                }
            }
            if (!wr) D(0) = (uint32_t)n;
            return 0;
        }
        case 0x09: /* DC_RdLI */
            if (d3 <= 0 || d3 >= MAXLCT || !lcts[d3].used) return E_UNID;
            {
                LCT *l = &lcts[d3];
                int line = lct_phys_line(l, d4);
                D(0) = (line >= 0 && line < l->lines && d5 >= 0 && d5 < LCT_COLS) ? l->ins[line * LCT_COLS + d5] : 0;
            }
            return 0;
        case 0x0A: /* DC_WrLI id, line, col, instr */
            if (d3 <= 0 || d3 >= MAXLCT || !lcts[d3].used) return E_UNID;
            dc_write_lct(d3, lct_phys_line(&lcts[d3], d4), d5, D(6));
            if (log_level >= 2) TRACE("DC_WrLI field %u lct %d line %d col %d ins %08x (lct res %d lines %d)",
                                      (unsigned)field_count, d3, d4, d5, D(6), lcts[d3].res, lcts[d3].lines);
            return 0;
        case 0x0B: /* DC_DlLCT */
            if (d3 > 0 && d3 < MAXLCT && lcts[d3].used) { emu_free(lcts[d3].ins); lcts[d3].used = 0; }
            return 0;
        case 0x0C: /* DC_FLnk fct, lct, line */
            if (d3 <= 0 || d3 >= MAXFCT || !fcts[d3].used) return E_UNID;
            if (d4 <= 0 || d4 >= MAXLCT || !lcts[d4].used) return E_UNID;
            fcts[d3].lct = d4;
            fcts[d3].lct_line = lct_phys_line(&lcts[d4], d5);
            TRACE("DC_FLnk fct %d -> lct %d line %d", d3, d4, d5);
            return 0;
        case 0x0D: /* DC_LLnk lct1, line1, lct2, line2 */
            if (d3 <= 0 || d3 >= MAXLCT || !lcts[d3].used) return E_UNID;
            if (d5 <= 0 || d5 >= MAXLCT || !lcts[d5].used) return E_UNID;
            dc_write_lct(d3, lct_phys_line(&lcts[d3], d4), LCT_COLS - 1,
                         LINK_OP | ((uint32_t)d5 << 12) | (uint32_t)lct_phys_line(&lcts[d5], d6));
            TRACE("DC_LLnk lct %d line %d -> lct %d line %d", d3, d4, d5, d6);
            return 0;
        case 0x0E: /* DC_Exec fctA, fctB */
            exec_fct[0] = d3;
            exec_fct[1] = d4;
            TRACE("DC_Exec %d %d", d3, d4);
            return 0;
        case 0x11: /* DC_SSig signal, count */
            ssig_sig = D(3) & 0xFFFF;
            ssig_count = d4;
            ssig_pid = os9_cur_pid();
            return 0;
        case 0x12: /* DC_Relea */
            ssig_count = -1;
            return 0;
        case 0x10: /* DC_NOP lct, line, col, lines, cols: fill a region with NOPs */
            /* Voyeur clears its plane A panning table this way before it shows
               a zoomed DYUV picture; stale per-line VSR loads gave noise */
            if (d3 <= 0 || d3 >= MAXLCT || !lcts[d3].used) return E_UNID;
            {
                LCT *l = &lcts[d3];
                for (int y = 0; y < d6; y++) {
                    int line = lct_phys_line(l, d4 + y);
                    for (int x = 0; x < d7; x++) {
                        int col = d5 + x;
                        if (line < 0 || line >= l->lines || col < 0 || col >= LCT_COLS) continue;
                        l->ins[line * LCT_COLS + col] = 0x10000000;
                    }
                }
                TRACE("DC_NOP field %u lct %d line %d col %d lines %d cols %d", field_count, d3, d4, d5, d6, d7);
            }
            return 0;
        case 0x0F: case 0x13:
            TRACE("DC fn %x (%d) ignored", fn, d3);
            if (fn == 0x13) D(0) = 0;
            return 0;
        default:
            LOG("DC function %x not implemented (d3=%d d4=%d d5=%d d6=%d d7=%d)", fn, d3, d4, d5, d6, d7);
            return 0;
        }
    }
    if (code == 0x59) return ptr_setstat(code);
    if (code == 0x52) { /* SS_GC: graphics cursor */
        switch (fn) {
        case 0: gc.hx = (int16_t)(D(3) >> 16); gc.hy = (int16_t)D(3); break;      /* GC_Pos */
        case 1: gc.show = 1; break;                                               /* GC_Show */
        case 2: gc.show = 0; break;                                               /* GC_Hide */
        case 3: {                                                                 /* GC_Ptn */
            gc.hotx = (int16_t)(D(3) >> 16); gc.hoty = (int16_t)D(3);
            gc.w = (int16_t)(D(4) >> 16); gc.h = (int16_t)D(4);
            gc.res = D(5) & 0xFFFF;
            if (gc.h > 32) gc.h = 32;
            int wpl = (gc.w + 15) / 16;
            for (int y = 0; y < gc.h; y++) gc.ptn[y] = wpl ? RW(A(0) + y * wpl * 2) : 0;
            break;
        }
        case 4: gc.color = D(3); break;                                           /* GC_Col */
        case 5: gc.ox = (int16_t)(D(3) >> 16); gc.oy = (int16_t)D(3); break;      /* GC_Org */
        case 6: gc.blink = D(3) & 0xFFFF; break;                                  /* GC_Blnk */
        }
        TRACE("GC fn %x d3=%08x", fn, D(3));
        return 0;
    }
    LOG("UCM setstat %x fn %x not implemented", code, fn);
    return E_UNKSVC;
}

int ptr_getstat(uint32_t code);
int ptr_setstat(uint32_t code);

int ucm_getstat(uint32_t code) {
    uint32_t fn = D(2) & 0xFFFF;
    if (code == 0x59) return ptr_getstat(code);
    if (code == 0x56 && fn == 0x03) return ucm_setstat(code);
    LOG("UCM getstat %x fn %x not implemented", code, fn);
    return E_UNKSVC;
}

/* ---------------------------------------------------------------- pointer */
/* InputState input lives on the host side (host_win32.c): it is not rolled back */
static int ptr_x = 384, ptr_y = 240;
static int ptr_sig, ptr_sig_pid = 1;

int ptr_getstat(uint32_t code) {
    uint32_t fn = D(2) & 0xFFFF;
    if (code == 0x59 && fn == 0) { /* PT_Coord */
        D(0) = 0x8000u | (input.b1 ? 1 : 0) | (input.b2 ? 2 : 0) | (input.b3 ? 3 : 0);
        D(1) = ((uint32_t)(uint16_t)ptr_x << 16) | (uint16_t)ptr_y;
        if (log_level >= 2 && (D(0) & 3)) TRACE("PT_Coord buttons %x at %d,%d", D(0) & 3, ptr_x, ptr_y);
        return 0;
    }
    LOG("PTR getstat %x fn %x", code, fn);
    return E_UNKSVC;
}

int ptr_setstat(uint32_t code) {
    uint32_t fn = D(2) & 0xFFFF;
    if (code == 0x59) {
        switch (fn) {
        case 1: ptr_sig = D(3) & 0xFFFF; ptr_sig_pid = os9_cur_pid(); return 0;  /* PT_SSig */
        case 2: ptr_sig = 0; return 0;              /* PT_Relea */
        case 4: return 0;                            /* PT_Org */
        case 3:                                      /* PT_Pos */
            ptr_x = (int16_t)(D(3) >> 16);
            ptr_y = (int16_t)D(3);
            return 0;
        }
    }
    LOG("PTR setstat %x fn %x", code, fn);
    return E_UNKSVC;
}

static void pointer_field(void) {
    static int ob1, ob2;
    int moved = input.dx || input.dy;
    ptr_x += input.dx;
    ptr_y += input.dy;
    if (ptr_x < 0) ptr_x = 0;
    if (ptr_x > 767) ptr_x = 767;
    if (ptr_y < 0) ptr_y = 0;
    if (ptr_y > 479) ptr_y = 479;
    if (ptr_sig && (moved || ob1 != input.b1 || ob2 != input.b2)) sig_send_pid(ptr_sig_pid, (uint32_t)ptr_sig);
    ob1 = input.b1; ob2 = input.b2;
}

/* ---------------------------------------------------------------- render */
static FORCEINLINE int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void exec_ins(int plane, uint32_t ins) {
    uint32_t op = ins >> 24, prm = ins & 0xFFFFFF;
    Plane *P = &vs.p[plane];
    if (op >= 0x80 && op <= 0xBF) {
        int bank = P->clut_bank & 3;
        vs.clut[bank * 64 + (op - 0x80)] = prm;
        return;
    }
    switch (op) {
    case 0x10: break;
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        P->vsr = prm; P->vsr_set = 1; break;
    case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67:
    case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F:
        vs.interrupts++; break;
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F:
        P->ddr = prm; break;
    case 0xC0: if (plane == 0) vs.icm = prm; break;
    case 0xC1: if (plane == 0) vs.tcr = prm; break;
    case 0xC2: if (plane == 0) vs.porder = prm; break;
    case 0xC3: P->clut_bank = prm & 3; break;
    case 0xC4: vs.p[0].transp_color = prm; break;
    case 0xC6: vs.p[1].transp_color = prm; break;
    case 0xC7: vs.p[0].mask_color = prm; break;
    case 0xC9: vs.p[1].mask_color = prm; break;
    case 0xCA: vs.p[0].dyuv_abs = prm; break;
    case 0xCB: vs.p[1].dyuv_abs = prm; break;
    case 0xD0: case 0xD1: case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: case 0xD7:
        vs.matte[op - 0xD0] = prm; break;
    case 0xD8: vs.backdrop = prm & 15; break;
    case 0xD9: vs.p[0].mosaic = prm & 0xFF; vs.p[0].mosaic_en = (prm >> 23) & 1; break;
    case 0xDA: vs.p[1].mosaic = prm & 0xFF; vs.p[1].mosaic_en = (prm >> 23) & 1; break;
    case 0xDB: vs.p[0].icf = prm & 63; break;
    case 0xDC: vs.p[1].icf = prm & 63; break;
    default:
        if ((op & 0xF0) == 0x20) break;   /* link handled by caller */
        break;
    }
}

/* DYUV quantization table (Green Book Figure V.18) */
static const int dyuv_q[16] = { 0, 1, 4, 9, 16, 27, 44, 79, 128, 177, 212, 229, 240, 247, 252, 255 };

/* decode one line of a plane into idx[] (CLUT index, or -1 transparent-free RGB in rgb[]) */
typedef struct {
    int mode;              /* 0 off, 1 clut8/rgb555, 3 clut7, 4 dual clut7, 5 dyuv, 11 clut4 */
    uint32_t rgb[SCREEN_W];
    uint8_t is_tbit[SCREEN_W];  /* RGB555 transparency bit */
    uint8_t idx_valid;
    uint8_t natural;       /* photographic coding (RL7 video, DYUV): upscaled by the renderer */
} LineBuf;

static LineBuf lb[2];

static uint32_t yuv_to_rgb(int y, int u, int v) {
    int b = y + (u - 128) * 1733 / 1000;
    int r = y + (v - 128) * 1371 / 1000;
    int g = (y * 1000 - 299 * r - 114 * b) / 587;
    return ((uint32_t)clampi(r, 0, 255) << 16) | ((uint32_t)clampi(g, 0, 255) << 8) | (uint32_t)clampi(b, 0, 255);
}

static void decode_line(int plane) {
    Plane *P = &vs.p[plane];
    LineBuf *L = &lb[plane];
    int cm = plane == 0 ? (vs.icm & 15) : ((vs.icm >> 8) & 15);
    L->mode = cm;
    uint32_t a = P->vsr;
    int rms = P->ddr & 3;
    int bp = (P->ddr >> 8) & 3;
    L->natural = cm == 5 || (rms == 2 && cm != 11);
    memset(L->is_tbit, 0, sizeof L->is_tbit);
    if (cm == 0) return;

    int clut_base = 0;
    if (cm == 3) clut_base = plane == 0 ? 0 : 128;
    if (cm == 4) clut_base = (vs.icm & 0x400000) ? 128 : 0;
    if (cm == 11) clut_base = plane == 0 ? 0 : 128;

    if (rms == 2) {
        /* run-length coded: RL7 (8 bpp) or RL3 (4 bpp pairs) */
        int x = 0;
        int dbl = (cm == 11);
        int maxx = dbl ? 384 : 384;   /* units: pixels (RL7) or pixel pairs (RL3) */
        int guard = 0;
        uint32_t a0 = a;
        while (x < maxx && guard++ < 2000) {
            uint8_t b = RB(a++);
            int run = 1;
            if (b & 0x80) {
                run = RB(a++);
                if (run == 0) run = maxx - x;
            }
            for (int k = 0; k < run && x < maxx; k++, x++) {
                if (!dbl) {
                    uint32_t c = vs.clut[(clut_base + (b & 0x7F)) & 255];
                    L->rgb[x * 2] = L->rgb[x * 2 + 1] = c;
                } else {
                    uint32_t c0 = vs.clut[(clut_base + ((b >> 4) & 7)) & 255];
                    uint32_t c1 = vs.clut[(clut_base + (b & 7)) & 255];
                    L->rgb[x * 2] = c0; L->rgb[x * 2 + 1] = c1;
                }
            }
        }
        P->vsr = a;
        if (!dbl && a > a0 && a - a0 < 4096) {
            g_vfi.hash[plane] = fnv64(g_vfi.hash[plane], RAM + (a0 & MEM_MASK), a - a0);
            g_vfi.lines[plane]++;
            g_vfi.coding[plane] = 0x47;
            g_vfi.line_hash[plane][g_vfi.cur_line] = fnv64(FNV64_INIT, RAM + (a0 & MEM_MASK), a - a0);
        }
        return;
    }
    uint32_t a0 = a;
    switch (cm) {
    case 1:
        if (plane == 0) { /* CLUT8 */
            for (int x = 0; x < 384; x++) {
                uint32_t c = vs.clut[RB(a++)];
                L->rgb[x * 2] = L->rgb[x * 2 + 1] = c;
            }
        } else { /* RGB555: upper byte from plane A store (we use same addr scheme) */
            uint32_t aa = vs.p[0].vsr;
            for (int x = 0; x < 384; x++) {
                uint8_t hi = RB(aa++), lo = RB(a++);
                uint32_t v = ((uint32_t)hi << 8) | lo;
                int r = ((v >> 10) & 31) * 8, g = ((v >> 5) & 31) * 8, bb = (v & 31) * 8;
                L->rgb[x * 2] = L->rgb[x * 2 + 1] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bb;
                L->is_tbit[x * 2] = L->is_tbit[x * 2 + 1] = (hi >> 7) & 1;
            }
        }
        break;
    case 3: case 4: /* CLUT7 */
        for (int x = 0; x < 384; x++) {
            uint32_t c = vs.clut[(clut_base + (RB(a++) & 0x7F)) & 255];
            L->rgb[x * 2] = L->rgb[x * 2 + 1] = c;
        }
        break;
    case 11: { /* CLUT4: 768 pixels from 384 bytes (double res) */
        int n = (bp == 1) ? 384 : 192;
        for (int x = 0; x < n; x++) {
            uint8_t b = RB(a++);
            uint32_t c0 = vs.clut[(clut_base + (b >> 4)) & 255], c1 = vs.clut[(clut_base + (b & 15)) & 255];
            if (n == 384) { L->rgb[x * 2] = c0; L->rgb[x * 2 + 1] = c1; }
            else { L->rgb[x * 4] = L->rgb[x * 4 + 1] = c0; L->rgb[x * 4 + 2] = L->rgb[x * 4 + 3] = c1; }
        }
        break;
    }
    case 5: { /* DYUV */
        int y = (P->dyuv_abs >> 16) & 255, u = (P->dyuv_abs >> 8) & 255, v = P->dyuv_abs & 255;
        int pu = u, pv = v;
        for (int x = 0; x < 384; x += 2) {
            uint8_t b1 = RB(a++), b2 = RB(a++);
            int du = b1 >> 4, dy0 = b1 & 15, dv = b2 >> 4, dy1 = b2 & 15;
            int nu = (u + dyuv_q[du]) & 255, nv = (v + dyuv_q[dv]) & 255;
            int y0 = (y + dyuv_q[dy0]) & 255;
            int y1 = (y0 + dyuv_q[dy1]) & 255;
            /* interpolate chroma for the second pixel */
            uint32_t c0 = yuv_to_rgb(y0, (pu + nu) / 2, (pv + nv) / 2);
            uint32_t c1 = yuv_to_rgb(y1, nu, nv);
            L->rgb[x * 2] = L->rgb[x * 2 + 1] = c0;
            L->rgb[x * 2 + 2] = L->rgb[x * 2 + 3] = c1;
            pu = nu; pv = nv;
            u = nu; v = nv; y = y1;
        }
        g_vfi.hash[plane] = fnv64(g_vfi.hash[plane], RAM + (a0 & MEM_MASK), a - a0);
        g_vfi.lines[plane]++;
        g_vfi.coding[plane] = 5;
        g_vfi.line_hash[plane][g_vfi.cur_line] = fnv64(FNV64_INIT, RAM + (a0 & MEM_MASK), a - a0);
        if (getenv("RECOMP_DYUVLOG") && (g_vfi.cur_line % 40) == 1)
            LOG("dyuv plane %d line %d abs %06x", plane, g_vfi.cur_line, P->dyuv_abs);
        break;
    }
    default:
        memset(L->rgb, 0, sizeof L->rgb);
        break;
    }
    P->vsr = a;
}

static FORCEINLINE int color_key(uint32_t c, uint32_t key, uint32_t mask) {
    uint32_t m = 0xFCFCFC & ~mask;
    return ((c ^ key) & m) == 0;
}

static FORCEINLINE int transparent(int tsel, uint32_t c, int tbit, int mf0, int mf1, uint32_t key, uint32_t mask) {
    int ck;
    switch (tsel) {
    case 0: return 1;
    case 1: return color_key(c, key, mask);
    case 2: return tbit;
    case 3: return mf0;
    case 4: return mf1;
    case 5: return mf0 || color_key(c, key, mask);
    case 6: return mf1 || color_key(c, key, mask);
    case 8: return 0;
    case 9: ck = color_key(c, key, mask); return !ck;
    case 10: return !tbit;
    case 11: return !mf0;
    case 12: return !mf1;
    case 13: return !mf0 || !color_key(c, key, mask);
    case 14: return !mf1 || !color_key(c, key, mask);
    }
    return 0;
}

static FORCEINLINE int apply_icf(int c, int icf) { return (icf * (c - 16)) / 63 + 16; }

static FORCEINLINE uint32_t to_host(int r, int g, int b) {
    r = clampi((r - 16) * 255 / 219, 0, 255);
    g = clampi((g - 16) * 255 / 219, 0, 255);
    b = clampi((b - 16) * 255 / 219, 0, 255);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static int hide_plane = -1;    /* RECOMP_HIDEPLANE=1|2: debugging */
static void compose_line(int y) {
    if (hide_plane < 0) hide_plane = getenv("RECOMP_HIDEPLANE") ? atoi(getenv("RECOMP_HIDEPLANE")) : 0;
    uint32_t *out = video_fb + y * SCREEN_W;
    int ta = vs.tcr & 15, tb = (vs.tcr >> 8) & 15;
    int mix = !((vs.tcr >> 23) & 1);
    int b_front = (vs.porder & 7) == 1;
    int bk = vs.backdrop;
    int kk = (bk & 8) ? 235 : 126;
    int bdr = (bk & 4) ? kk : 16, bdg = (bk & 2) ? kk : 16, bdb = (bk & 1) ? kk : 16;
    int icfa = vs.p[0].icf, icfb = vs.p[1].icf;
    int two_mattes = (vs.icm >> 19) & 1;
    /* matte processing */
    int mf[2] = {0, 0};
    int next_m[2] = {0, two_mattes ? 4 : 8};
    int end_m[2] = {two_mattes ? 4 : 8, 8};
    int hold[2] = {vs.p[0].mosaic_en ? vs.p[0].mosaic : 1, vs.p[1].mosaic_en ? vs.p[1].mosaic : 1};
    uint32_t heldc[2] = {0, 0};
#ifdef RECOMP_HD
    /* per-line composition parameters for the HD renderer */
    {
        HdLine *L = &g_hdf.line[y];
        int matte_on = (vs.matte[0] >> 20) & 15 || (two_mattes && ((vs.matte[4] >> 20) & 15));
        L->simple = (!mix && !matte_on && hold[0] <= 1 && hold[1] <= 1) ? 1.0f : 0.0f;
        L->b_front = (float)b_front;
        L->icf[0] = icfa / 63.0f; L->icf[1] = icfb / 63.0f;
        uint32_t bdc = to_host(bdr, bdg, bdb);
        L->bd[0] = ((bdc >> 16) & 255) / 255.0f; L->bd[1] = ((bdc >> 8) & 255) / 255.0f; L->bd[2] = (bdc & 255) / 255.0f;
        L->natural[0] = (float)(lb[0].mode && lb[0].natural);
        L->natural[1] = (float)(lb[1].mode && lb[1].natural);
    }
    uint32_t *hpa = g_hdf.plane[0] + y * SCREEN_W, *hpb = g_hdf.plane[1] + y * SCREEN_W;
    memset(g_hdf.overlay + y * SCREEN_W, 0, SCREEN_W * 4);
#endif
    for (int x = 0; x < SCREEN_W; x++) {
        for (int s = 0; s < (two_mattes ? 2 : 1); s++) {
            while (next_m[s] < end_m[s]) {
                uint32_t mc = vs.matte[next_m[s]];
                int op = (mc >> 20) & 15;
                int pos = mc & 0x3FF;
                if (op == 0) { next_m[s] = end_m[s]; break; }
                if (pos > x) break;
                int flag = two_mattes ? s : ((mc >> 16) & 1);
                if (op & 8) mf[flag] = op & 1;
                if ((op & 6) == 4) icfa = (mc >> 10) & 63;
                if ((op & 6) == 6) icfb = (mc >> 10) & 63;
                next_m[s]++;
            }
        }
        uint32_t ca = lb[0].rgb[x], cb = lb[1].rgb[x];
        if (hold[0] > 1) { if ((x / 2) % hold[0] == 0 && !(x & 1)) heldc[0] = ca; ca = heldc[0]; }
        if (hold[1] > 1) { if ((x / 2) % hold[1] == 0 && !(x & 1)) heldc[1] = cb; cb = heldc[1]; }
        int tra = hide_plane == 1 || lb[0].mode == 0 || transparent(ta, ca, lb[0].is_tbit[x], mf[0], mf[1], vs.p[0].transp_color, vs.p[0].mask_color);
        int trb = hide_plane == 2 || lb[1].mode == 0 || transparent(tb, cb, lb[1].is_tbit[x], mf[0], mf[1], vs.p[1].transp_color, vs.p[1].mask_color);
        int ar = apply_icf((ca >> 16) & 255, icfa), ag = apply_icf((ca >> 8) & 255, icfa), ab = apply_icf(ca & 255, icfa);
        int br = apply_icf((cb >> 16) & 255, icfb), bg = apply_icf((cb >> 8) & 255, icfb), bb = apply_icf(cb & 255, icfb);
        int r, g, b, nat;
        if (mix) {
            if (tra) { ar = ag = ab = 16; }
            if (trb) { br = bg = bb = 16; }
            if (tra && trb) { r = bdr; g = bdg; b = bdb; nat = 0; }
            else { r = ar + br - 16; g = ag + bg - 16; b = ab + bb - 16;
                   nat = (tra || lb[0].natural) && (trb || lb[1].natural); }
        } else {
            if (b_front) {
                if (!trb) { r = br; g = bg; b = bb; nat = lb[1].natural; }
                else if (!tra) { r = ar; g = ag; b = ab; nat = lb[0].natural; }
                else { r = bdr; g = bdg; b = bdb; nat = 0; }
            } else {
                if (!tra) { r = ar; g = ag; b = ab; nat = lb[0].natural; }
                else if (!trb) { r = br; g = bg; b = bb; nat = lb[1].natural; }
                else { r = bdr; g = bdg; b = bdb; nat = 0; }
            }
        }
        /* alpha byte marks video/photo pixels for the GPU upscaler (ignored by GDI/BMP) */
        out[x] = to_host(clampi(r, 0, 255), clampi(g, 0, 255), clampi(b, 0, 255)) | (nat ? 0xFF000000u : 0);
#ifdef RECOMP_HD
        hpa[x] = to_host((ca >> 16) & 255, (ca >> 8) & 255, ca & 255) | (tra ? 0 : 0xFF000000u);
        hpb[x] = to_host((cb >> 16) & 255, (cb >> 8) & 255, cb & 255) | (trb ? 0 : 0xFF000000u);
#endif
    }
}

static void draw_cursor(void) {
    if (!gc.show) return;
    if (gc.blink) {
        int on = (gc.blink >> 8) & 0x7F, off = gc.blink & 0x7F;
        if (off) {
            unsigned period = 12u * (unsigned)(on + off);
            if (field_count % period >= 12u * (unsigned)on) {
                if (!(gc.blink & 0x8000)) return;   /* normal blink: transparent when off */
            }
        }
    }
    int I = (gc.color >> 31) & 1;
    int k = I ? 255 : 127;
    uint32_t col = ((gc.color >> 23) & 1 ? (uint32_t)k << 16 : 0) |
                   ((gc.color >> 15) & 1 ? (uint32_t)k << 8 : 0) |
                   ((gc.color >> 7) & 1 ? (uint32_t)k : 0);
    /* positions are in high resolution (768 x 480) */
    int px = gc.ox + gc.hx - gc.hotx;
    int py = gc.oy + gc.hy - gc.hoty;
    int xs = gc.res == 0 ? 2 : 1;          /* pattern pixel width in double-res pixels */
    for (int y = 0; y < gc.h && y < 32; y++) {
        int sy = (py + y * (gc.res == 2 ? 1 : 2)) / 2;
        if (sy < 0 || sy >= SCREEN_LINES) continue;
        uint16_t bits = gc.ptn[y];
        for (int x = 0; x < 16; x++) {
            if (!(bits & (0x8000 >> x))) continue;
            for (int r = 0; r < xs; r++) {
                int sx = px + x * xs + r;
                if (sx >= 0 && sx < SCREEN_W) video_fb[sy * SCREEN_W + sx] = col;
#ifdef RECOMP_HD
                if (sx >= 0 && sx < SCREEN_W) g_hdf.overlay[sy * SCREEN_W + sx] = col | 0xFF000000u;
#endif
            }
        }
    }
}

void video_init(void) {
    video_fb = (uint32_t *)calloc(SCREEN_W * SCREEN_LINES, 4);
    memset(&vs, 0, sizeof vs);
    vs.p[0].icf = vs.p[1].icf = 63;
}

void video_field(void) {
    for (int p = 0; p < 2; p++) { g_vfi.lines[p] = 0; g_vfi.coding[p] = 0; g_vfi.hash[p] = FNV64_INIT; }
    memset(g_vfi.line_hash, 0, sizeof g_vfi.line_hash);
    pointer_field();
    vs.interrupts = 0;
    int cur_lct[2] = {0, 0}, cur_line[2] = {0, 0};
    /* field control tables */
    for (int p = 0; p < 2; p++) {
        vs.p[p].vsr_set = 0;
        int id = exec_fct[p];
        if (id <= 0 || id >= MAXFCT || !fcts[id].used) continue;
        FCT *f = &fcts[id];
        for (int i = 0; i < f->len; i++) exec_ins(p, f->ins[i]);
        cur_lct[p] = f->lct;
        cur_line[p] = f->lct_line;
    }
    int planes_on = (exec_fct[0] > 0) || (exec_fct[1] > 0);
    {   /* RECOMP_LCTDUMP=field: dump the line control tables in use (debugging) */
        static long dumpf = -2;
        if (dumpf == -2) dumpf = getenv("RECOMP_LCTDUMP") ? atol(getenv("RECOMP_LCTDUMP")) : -1;
        if ((long)field_count == dumpf) {
            for (int id = 1; id < MAXLCT; id++) {
                if (!lcts[id].used) continue;
                LOG("LCT %d plane %d lines %d res %d", id, lcts[id].plane, lcts[id].lines, lcts[id].res);
                for (int ln = 0; ln < lcts[id].lines && ln < 240; ln++) {
                    uint32_t *row = lcts[id].ins + ln * LCT_COLS;
                    if (getenv("RECOMP_LCTDUMP_ALL") == NULL && ln > 6 && ln < 236 && ln % 20) continue;
                    LOG("  %3d: %08x %08x %08x %08x %08x %08x %08x %08x", ln, row[0], row[1], row[2], row[3], row[4], row[5], row[6], row[7]);
                }
            }
            for (int id = 1; id < MAXFCT; id++) {
                if (!fcts[id].used) continue;
                char b[600]; int k = 0;
                for (int i = 0; i < fcts[id].len && k < 560; i++) k += sprintf(b + k, " %08x", fcts[id].ins[i]);
                LOG("FCT %d plane %d -> lct %d line %d:%s", id, fcts[id].plane, fcts[id].lct, fcts[id].lct_line, b);
            }
        }
    }
    {   /* RECOMP_CLUTLOG=field: print the colour table (debug) */
        static long clutf = -2;
        if (clutf == -2) clutf = getenv("RECOMP_CLUTLOG") ? atol(getenv("RECOMP_CLUTLOG")) : -1;
        if ((long)field_count == clutf)
            for (int i = 0; i < 256; i += 8)
                LOG("CLUT %3d: %06x %06x %06x %06x %06x %06x %06x %06x", i, vs.clut[i], vs.clut[i + 1], vs.clut[i + 2],
                    vs.clut[i + 3], vs.clut[i + 4], vs.clut[i + 5], vs.clut[i + 6], vs.clut[i + 7]);
    }
    static int vidlog = -1;
    if (vidlog < 0) vidlog = getenv("RECOMP_VIDLOG") != NULL;
    uint32_t vsr_line0[2] = {0, 0};
    for (int y = 0; y < SCREEN_LINES; y++) {
        for (int p = 0; p < 2; p++) {
            int id = cur_lct[p];
            if (id <= 0 || id >= MAXLCT || !lcts[id].used) continue;
            LCT *l = &lcts[id];
            int line = cur_line[p];
            int next_id = id, next_line = line + 1;
            if (line >= 0 && line < l->lines) {
                uint32_t *row = l->ins + line * LCT_COLS;
                for (int c = 0; c < LCT_COLS; c++) {
                    uint32_t ins = row[c];
                    if ((ins >> 28) == 2) {
                        next_id = (ins >> 12) & 0xFFF;
                        next_line = ins & 0xFFF;
                        continue;
                    }
                    exec_ins(p, ins);
                }
            }
            cur_lct[p] = next_id;
            cur_line[p] = next_line;
        }
        if (y == 0) { vsr_line0[0] = vs.p[0].vsr; vsr_line0[1] = vs.p[1].vsr; }
        if (y == 120 && vidlog)
            LOG("field %u fct %d/%d icm %06x tcr %06x porder %x icf %d/%d ddr %06x/%06x vsr0 %06x/%06x lct %d/%d tc %06x/%06x bd %x",
                field_count, exec_fct[0], exec_fct[1], vs.icm, vs.tcr, vs.porder, vs.p[0].icf, vs.p[1].icf,
                vs.p[0].ddr, vs.p[1].ddr, vsr_line0[0], vsr_line0[1], cur_lct[0], cur_lct[1],
                vs.p[0].transp_color, vs.p[1].transp_color, vs.backdrop);
        g_vfi.cur_line = y;
        if (planes_on) {
            decode_line(0);
            decode_line(1);
            compose_line(y);
        } else {
            memset(video_fb + y * SCREEN_W, 0, SCREEN_W * 4);
        }
    }
    /* video interrupts -> DC_SSig */
    int irq = vs.interrupts;
    if (irq == 0 && planes_on) irq = 0;
    for (int k = 0; k < irq && ssig_count >= 0; k++) {
        if (ssig_count == 0) {
            int s = ssig_sig;
            ssig_count = -1;
            sig_send_pid(ssig_pid, (uint32_t)s);
        } else ssig_count--;
    }
    {   /* RECOMP_HASHLOG=1: log picture hashes when they change (debug) */
        static int hl = -1;
        static uint64_t last[2];
        if (hl < 0) hl = getenv("RECOMP_HASHLOG") != NULL;
        if (hl && (g_vfi.hash[0] != last[0] || g_vfi.hash[1] != last[1]))
            LOG("field %u hash A %d:%d %016llx B %d:%d %016llx | A60 %016llx A120 %016llx A180 %016llx B60 %016llx B120 %016llx B180 %016llx",
                field_count, g_vfi.coding[0], g_vfi.lines[0],
                (unsigned long long)g_vfi.hash[0], g_vfi.coding[1], g_vfi.lines[1], (unsigned long long)g_vfi.hash[1],
                (unsigned long long)g_vfi.line_hash[0][60], (unsigned long long)g_vfi.line_hash[0][120],
                (unsigned long long)g_vfi.line_hash[0][180], (unsigned long long)g_vfi.line_hash[1][60],
                (unsigned long long)g_vfi.line_hash[1][120], (unsigned long long)g_vfi.line_hash[1][180]);
        last[0] = g_vfi.hash[0]; last[1] = g_vfi.hash[1];
    }
    field_count++;
    draw_cursor();
    /* presentation, input and run-ahead happen at the frame boundary, where
       the CPU loop is back at top level (frame.c) */
    g_frame_ready = 1;
}
