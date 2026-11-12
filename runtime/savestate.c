/* In-memory save states for run-ahead (see emu_state.h).

   A snapshot is: the .emud/.emub sections (copied whole: a few hundred KB),
   and the pages of emulated RAM and of the machine heap that changed since
   the previous snapshot or restore.  Both RAM and heap are allocated with
   MEM_WRITE_WATCH so Windows tracks the dirty pages for us; a shadow copy
   of each region always equals the region as of the last snapshot. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "runtime.h"
#define EMU_HOST_SIDE   /* host code: ordinary sections */
#include "emu_state.h"
#include "savestate.h"

int g_speculative;

typedef struct {
    uint8_t *base, *shadow;
    size_t size;
    void **pages;          /* GetWriteWatch output */
} Region;

static Region regions[2];   /* 0 RAM, 1 machine heap */
static int nregions;
static struct { uint8_t *base, *shadow; size_t size; } secs[4];
static int nsecs;
static int ready;

static uint8_t *region_new(size_t size) {
    size = (size + 0xFFFF) & ~(size_t)0xFFFF;
    Region *r = &regions[nregions++];
    r->base = (uint8_t *)VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
    r->shadow = (uint8_t *)VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!r->base || !r->shadow) fatal("out of memory for the machine state");
    r->size = size;
    r->pages = (void **)VirtualAlloc(NULL, (size / 4096 + 1) * sizeof(void *), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return r->base;
}

/* ---------------------------------------------------------------- machine heap */
#define HEAP_SIZE (256u << 20)
typedef struct { size_t size, next; int used, pad; } Hdr;    /* 32 bytes; size includes header */
typedef struct { size_t top, free_head; } HeapTop;
static uint8_t *heap;

static void heap_setup(void) {
    heap = region_new(HEAP_SIZE);
    HeapTop *t = (HeapTop *)heap;
    t->top = 64;
    t->free_head = 0;
}

void *emu_alloc(size_t n) {
    if (!heap) heap_setup();
    HeapTop *t = (HeapTop *)heap;
    size_t need = ((n + 15) & ~(size_t)15) + sizeof(Hdr);
    size_t *pp = &t->free_head;
    while (*pp) {
        Hdr *h = (Hdr *)(heap + *pp);
        if (h->size >= need) {
            size_t off = *pp;
            if (h->size - need >= 256) {           /* split */
                Hdr *rest = (Hdr *)(heap + off + need);
                rest->size = h->size - need;
                rest->next = h->next;
                rest->used = 0;
                h->size = need;
                *pp = off + need;
            } else {
                *pp = h->next;
            }
            h->used = 1;
            memset(h + 1, 0, h->size - sizeof(Hdr));
            return h + 1;
        }
        pp = &h->next;
    }
    if (t->top + need > HEAP_SIZE) fatal("machine heap exhausted");
    Hdr *h = (Hdr *)(heap + t->top);
    h->size = need;
    h->used = 1;
    t->top += need;
    memset(h + 1, 0, need - sizeof(Hdr));
    return h + 1;
}

void emu_free(void *p) {
    if (!p) return;
    HeapTop *t = (HeapTop *)heap;
    Hdr *h = (Hdr *)p - 1;
    size_t off = (size_t)((uint8_t *)h - heap);
    h->used = 0;
    /* insert sorted by address and merge neighbours */
    size_t *pp = &t->free_head, prev = 0;
    while (*pp && *pp < off) { prev = *pp; pp = &((Hdr *)(heap + *pp))->next; }
    h->next = *pp;
    *pp = off;
    if (h->next && off + h->size == h->next) {
        Hdr *n = (Hdr *)(heap + h->next);
        h->size += n->size;
        h->next = n->next;
    }
    if (prev) {
        Hdr *ph = (Hdr *)(heap + prev);
        if (prev + ph->size == off) { ph->size += h->size; ph->next = h->next; }
    }
}

/* ---------------------------------------------------------------- RAM */
uint8_t *state_alloc_ram(size_t size) {
    if (!heap) heap_setup();
    return region_new(size);
}

/* ---------------------------------------------------------------- sections */
static void find_sections(void) {
    uint8_t *mod = (uint8_t *)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)mod;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(mod + dos->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections && nsecs < 4; i++) {
        char nm[9] = {0};
        memcpy(nm, s[i].Name, 8);
        if (!strcmp(nm, ".emud") || !strcmp(nm, ".emub")) {
            secs[nsecs].base = mod + s[i].VirtualAddress;
            secs[nsecs].size = s[i].Misc.VirtualSize;
            secs[nsecs].shadow = (uint8_t *)malloc(secs[nsecs].size);
            LOG("machine state section %s: %u bytes", nm, (unsigned)secs[nsecs].size);
            nsecs++;
        }
    }
    if (nsecs < 2) LOG("warning: machine state sections not found (%d); run-ahead disabled", nsecs);
}

int state_available(void) {
    if (!ready) { find_sections(); ready = 1; }
    return nsecs >= 2;
}

/* ---------------------------------------------------------------- save / load */
void state_save(void) {
    if (!state_available()) return;
    for (int i = 0; i < nregions; i++) {
        Region *r = &regions[i];
        ULONG_PTR n = r->size / 4096 + 1;
        ULONG gran = 4096;
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, r->base, r->size, r->pages, &n, &gran) != 0) {
            memcpy(r->shadow, r->base, r->size);    /* fallback: full copy */
            continue;
        }
        for (ULONG_PTR k = 0; k < n; k++) {
            size_t off = (size_t)((uint8_t *)r->pages[k] - r->base);
            memcpy(r->shadow + off, r->base + off, gran);
        }
    }
    for (int i = 0; i < nsecs; i++) memcpy(secs[i].shadow, secs[i].base, secs[i].size);
}

void state_load(void) {
    if (!state_available()) return;
    for (int i = 0; i < nregions; i++) {
        Region *r = &regions[i];
        ULONG_PTR n = r->size / 4096 + 1;
        ULONG gran = 4096;
        if (GetWriteWatch(0, r->base, r->size, r->pages, &n, &gran) != 0) {
            memcpy(r->base, r->shadow, r->size);
        } else {
            for (ULONG_PTR k = 0; k < n; k++) {
                size_t off = (size_t)((uint8_t *)r->pages[k] - r->base);
                memcpy(r->base + off, r->shadow + off, gran);
            }
        }
        ResetWriteWatch(r->base, r->size);
    }
    for (int i = 0; i < nsecs; i++) memcpy(secs[i].base, secs[i].shadow, secs[i].size);
}
