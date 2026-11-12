/* High-level emulation of the OS-9/68K (CD-RTOS) kernel for a single process. */
#include "recomp.h"
#include "runtime.h"
#include "game_config.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#endif
#include "emu_state.h"   /* machine state: must follow the other includes */

/* OS-9 error codes */
#define E_PTHFUL 0xC8
#define E_BPNUM_ 0xC9
#define E_BMODE_ 0xCB
#define E_MEMFUL 0xCF
#define E_UNKSVC_ 0xD0
#define E_EOF_   0xD3
#define E_PNNF_  0xD8
#define E_MNF    0xDD
#define E_NOCHLD 0xE2
#define E_IPRCID 0xE0

#define MODULE_BASE MAIN_MODULE_BASE   /* fixed load address of the recompiled main program */

/* modules that were statically recompiled for a fixed address (game.json) */
static uint32_t fixed_module_base(const char *name) {
    for (int i = 0; game_fixed_modules[i].name; i++)
        if (!_stricmp(game_fixed_modules[i].name, name)) return game_fixed_modules[i].base;
    return 0;
}

/* ------------------------------------------------------------ allocator */
typedef struct Block { uint32_t addr, size; struct Block *next; } Block;
static Block *free_list;

static void heap_init(void) {
    free_list = (Block *)emu_alloc(sizeof(Block));
    free_list->addr = HEAP_BASE;
    free_list->size = HEAP_END - HEAP_BASE;
}

uint32_t mem_alloc(uint32_t size, uint32_t *actual) {
    size = (size + 15) & ~15u;
    if (size == 0) size = 16;
    for (Block **pp = &free_list; *pp; pp = &(*pp)->next) {
        Block *b = *pp;
        if (b->size >= size) {
            uint32_t a = b->addr;
            b->addr += size;
            b->size -= size;
            if (b->size == 0) { *pp = b->next; emu_free(b); }
            memset(RAM + a, 0, size);
            if (actual) *actual = size;
            return a;
        }
    }
    return 0;
}

void mem_free(uint32_t addr, uint32_t size) {
    size = (size + 15) & ~15u;
    if (!addr || !size) return;
    Block *nb = (Block *)emu_alloc(sizeof(Block));
    nb->addr = addr; nb->size = size;
    Block **pp = &free_list;
    while (*pp && (*pp)->addr < addr) pp = &(*pp)->next;
    nb->next = *pp;
    *pp = nb;
    /* coalesce */
    for (Block *b = free_list; b && b->next;) {
        if (b->addr + b->size == b->next->addr) {
            Block *n = b->next;
            b->size += n->size;
            b->next = n->next;
            emu_free(n);
        } else b = b->next;
    }
}

/* ------------------------------------------------------------ signals */
#define MAXSIG 64
#define MAXPROC 4

typedef struct {
    uint32_t d[8], a[8], pc;
    uint8_t ccr, srh;
} Context;

/* Per-process kernel state.  Only one process runs at a time: a forked
   child runs to completion while its parent blocks in F$Wait. */
typedef struct {
    int used, pid;
    uint32_t q[MAXSIG];
    int nq;
    uint32_t handler, handler_a6;
    int mask;                       /* F$SigMask level + in-handler */
    Context saved[16];
    int nsaved;
} Proc;
static Proc procs[MAXPROC];
static Proc *cur;

#define sigq (cur->q)
#define sig_pending_count (cur->nq)
#define icpt_pc (cur->handler)
#define icpt_a6 (cur->handler_a6)
#define sig_masked (cur->mask)
#define ctx_stack (cur->saved)
#define ctx_depth (cur->nsaved)

int os9_cur_pid(void) { return cur ? cur->pid : 1; }

static Proc *proc_by_pid(int pid) {
    for (int i = 0; i < MAXPROC; i++)
        if (procs[i].used && procs[i].pid == pid) return &procs[i];
    return NULL;
}

void sig_send_pid(int pid, uint32_t sig) {
    Proc *p = proc_by_pid(pid);
    if (!p) return;
    if (p->nq < MAXSIG) p->q[p->nq++] = sig;
    TRACE("signal %u queued for pid %d", sig, pid);
}

void sig_send(uint32_t sig) { sig_send_pid(os9_cur_pid(), sig); }

static void push_context(uint32_t resume_pc) {
    if (ctx_depth >= 16) fatal("signal context overflow");
    Context *c = &ctx_stack[ctx_depth++];
    memcpy(c->d, m68k.d, sizeof c->d);
    memcpy(c->a, m68k.a, sizeof c->a);
    c->pc = resume_pc;
    c->ccr = GET_CCR();
    c->srh = m68k.srh;
}

/* Deliver one pending signal: m68k.pc must hold the resume address. */
int sig_deliver(void) {
    if (!sig_pending_count || sig_masked || !icpt_pc) return 0;
    uint32_t sig = sigq[0];
    memmove(sigq, sigq + 1, (--sig_pending_count) * sizeof(uint32_t));
    push_context(m68k.pc);
    D(1) = sig;
    A(6) = icpt_a6;
    A(7) -= 0x50;           /* the kernel builds a frame on the user stack */
    m68k.pc = icpt_pc;
    sig_masked++;
    TRACE("deliver signal %u -> %06x", sig, icpt_pc);
    return 1;
}

static uint32_t do_rte(void) {
    if (!ctx_depth) fatal("F$RTE without signal context");
    Context *c = &ctx_stack[--ctx_depth];
    memcpy(m68k.d, c->d, sizeof c->d);
    memcpy(m68k.a, c->a, sizeof c->a);
    SET_CCR(c->ccr);
    m68k.srh = c->srh;
    if (sig_masked > 0) sig_masked--;
    return c->pc;
}

/* ------------------------------------------------------------ alarms */
typedef struct { int used, pid; uint32_t id, sig; int64_t when, period; } Alarm;
static Alarm alarms[16];
static uint32_t next_alarm_id = 1;
#define TICKS_PER_SEC 100
#define TICK_CYC (CPU_IPS / TICKS_PER_SEC)

static void alarms_check(void) {
    int64_t now = m68k.cyc;
    for (int i = 0; i < 16; i++) {
        Alarm *al = &alarms[i];
        if (!al->used || now < al->when) continue;
        sig_send_pid(al->pid, al->sig);
        if (al->period > 0) al->when += al->period;
        else al->used = 0;
    }
}

static int64_t alarms_next(void) {
    int64_t n = INT64_MAX;
    for (int i = 0; i < 16; i++)
        if (alarms[i].used && alarms[i].when < n) n = alarms[i].when;
    return n;
}

/* ------------------------------------------------------------ scheduler */
static int64_t next_field, next_sector;
static int64_t field_period, sector_period;

void sched_init(void) {
    field_period = CPU_IPS / FIELD_RATE;
    sector_period = CPU_IPS / SECTOR_RATE;
    next_field = field_period;
    next_sector = sector_period;
    m68k.cyc_limit = next_sector < next_field ? next_sector : next_field;
}

int64_t sched_now(void) { return m68k.cyc; }

int64_t audio_next_event(void);
void audio_event(void);

static void run_events(void) {
    for (;;) {
        int progressed = 0;
        if (m68k.cyc >= audio_next_event()) { audio_event(); progressed = 1; }
        if (m68k.cyc >= next_sector) { next_sector += sector_period; cdfm_sector_tick(); progressed = 1; }
        if (m68k.cyc >= next_field) { next_field += field_period; video_field(); audio_field_tick(); progressed = 1; }
        alarms_check();
        if (!progressed || g_frame_ready) break;     /* frame boundary first */
    }
    int64_t lim = next_field;
    if (next_sector < lim) lim = next_sector;
    int64_t an = alarms_next();
    if (an < lim) lim = an;
    an = audio_next_event();
    if (an < lim) lim = an;
    m68k.cyc_limit = lim;
}

void sched_service(void) {
    run_events();
    if (sig_pending_count && !sig_masked && icpt_pc) {
        sig_deliver();
        /* make translated code yield again soon so nested deliveries work */
    }
}

/* Called from blocking syscalls: advance time to the next event. */
void sched_wait_event(void) {
    if (m68k.cyc < m68k.cyc_limit) m68k.cyc = m68k.cyc_limit;
    run_events();
}

/* ------------------------------------------------------------ paths */
enum { P_NONE, P_CONS, P_CD, P_NVR, P_VIDEO, P_PTR, P_AUDIO, P_NULL, P_KB };
typedef struct {
    int type;
    int handle;          /* CDFM handle */
    char host[400];      /* NVR host file (opened per operation) */
    uint32_t pos;        /* NVR file position */
    char name[128];
} Path;
#define MAXPATH 32
static Path paths[MAXPATH];
char g_data_dir[512] = ".";

static void read_cstr(uint32_t a, char *out, int max) {
    int i = 0;
    for (; i < max - 1; i++) {
        uint8_t c = RB(a + i);
        if (c == 0 || c == '\r' || c == '\n' || c == ' ') break;
        out[i] = (char)c;
    }
    out[i] = 0;
}

static int path_alloc(void) {
    for (int i = 3; i < MAXPATH; i++)
        if (paths[i].type == P_NONE) return i;
    return -1;
}

static void nvr_host_path(const char *name, char *out, size_t n) {
    /* name is the part after "/nvr/" */
    snprintf(out, n, "%s/nvr_%s", g_data_dir, name);
}

#define NVR_SIZE 8192
static uint32_t nvr_free_bytes(void) {
    /* 8 KB NVRAM minus the files the game created (the CSD is kept outside the budget) */
    long used = 0;
#ifdef _WIN32
    char pat[600];
    snprintf(pat, sizeof pat, "%s/nvr_*", g_data_dir);
    struct _finddata_t fd;
    intptr_t h = _findfirst(pat, &fd);
    if (h != -1) {
        do {
            if (_stricmp(fd.name, "nvr_csd")) used += (long)fd.size + 64;
        } while (_findnext(h, &fd) == 0);
        _findclose(h);
    }
#endif
    return used >= NVR_SIZE ? 0 : (uint32_t)(NVR_SIZE - used);
}

/* returns path number or -error */
static int do_open(const char *pname, int mode, int create) {
    char lower[128];
    size_t L = strlen(pname);
    if (L >= sizeof lower) L = sizeof lower - 1;
    for (size_t i = 0; i <= L; i++) lower[i] = (char)tolower((unsigned char)pname[i]);
    lower[L] = 0;
    int p = path_alloc();
    if (p < 0) return -E_PTHFUL;
    Path *P = &paths[p];
    memset(P, 0, sizeof *P);
    snprintf(P->name, sizeof P->name, "%s", pname);

    const char *nm = lower;
    if (!strncmp(nm, "/nvr", 4)) {
        const char *f = nm + 4;
        if (*f == '/') f++;
        if (!*f) { P->type = P_NVR; return p; }     /* directory itself */
        char hp[600];
        nvr_host_path(f, hp, sizeof hp);
        snprintf(P->host, sizeof P->host, "%s", hp);
        FILE *fp = NULL;
        if (create) {
            if (!g_speculative) fp = fopen(hp, "wb");
        } else fp = fopen(hp, "rb");
        if (!fp && !(create && g_speculative)) { P->type = P_NONE; return -E_PNNF_; }
        if (fp) fclose(fp);
        P->pos = 0;
        P->type = P_NVR;
        return p;
    }
    if (!strncmp(nm, "/vid", 4) || !strncmp(nm, "/video", 6) || !strncmp(nm, "/dev/vid", 8)) {
        P->type = P_VIDEO;
        return p;
    }
    if (!strncmp(nm, "/ptr", 4) || !strncmp(nm, "/cdikeys", 8) || !strncmp(nm, "/pt", 3)) {
        P->type = P_PTR;
        return p;
    }
    if (!strncmp(nm, "/ap", 3) || !strncmp(nm, "/audio", 6) || !strncmp(nm, "/adpcm", 6)) {
        P->type = P_AUDIO;
        return p;
    }
    if (!strncmp(nm, "/nil", 4) || !strncmp(nm, "/term", 5) || !strncmp(nm, "/t1", 3)) {
        P->type = P_NULL;
        return p;
    }
    if (!strncmp(nm, "/kb", 3)) {
        P->type = P_KB;
        return p;
    }
    /* CD files: "/cd/name" or plain "name" relative to /cd */
    const char *f = nm;
    if (!strncmp(f, "/cd", 3)) {
        f += 3;
        if (*f == '/') f++;
        if (!*f) { P->type = P_AUDIO; return p; }   /* "/cd" device itself: audio functions */
    } else if (*f == '/') {
        LOG("I$Open of unknown device '%s'", pname);
        P->type = P_NONE;
        return -E_PNNF_;
    }
    int h = cdfm_open(f, mode);
    if (h < 0) { P->type = P_NONE; return -E_PNNF_; }
    P->type = P_CD;
    P->handle = h;
    return p;
}

/* ------------------------------------------------------------ modules */
typedef struct { char name[32]; uint32_t addr; uint32_t size; int links; } ModEntry;
static ModEntry mods[16];
static int nmods;

static uint32_t mod_register(const char *name, uint32_t addr, uint32_t size) {
    ModEntry *m = &mods[nmods++];
    snprintf(m->name, sizeof m->name, "%s", name);
    m->addr = addr; m->size = size; m->links = 1;
    return addr;
}

static ModEntry *mod_find(const char *name) {
    for (int i = 0; i < nmods; i++)
        if (!_stricmp(mods[i].name, name)) return &mods[i];
    return NULL;
}

static ModEntry *mod_load_from_disc(const char *name) {
    ModEntry *m = mod_find(name);
    if (m) { m->links++; return m; }
    const DiscFile *df = disc_find(name);
    if (!df) return NULL;
    uint8_t *buf; uint32_t len;
    if (disc_read_file(df, &buf, &len)) return NULL;
    /* modules that were statically recompiled must sit at their fixed address */
    uint32_t a = fixed_module_base(name);
    if (!a) a = mem_alloc(len, NULL);
    memcpy(RAM + a, buf, len);
    free(buf);
    /* a file may contain several modules; register each */
    uint32_t off = 0;
    ModEntry *first = NULL;
    while (off + 0x30 <= len && RW(a + off) == 0x4AFC) {
        uint32_t sz = RL(a + off + 4);
        char nm[32];
        read_cstr(a + off + RL(a + off + 12), nm, sizeof nm);
        mod_register(nm, a + off, sz);
        if (!first) first = &mods[nmods - 1];
        LOG("loaded module '%s' at %06x size %x", nm, a + off, sz);
        off += sz;
        off = (off + 3) & ~3u;
        if (!sz) break;
    }
    return first;
}

/* Set up a process for program module mb as the OS-9 kernel's F$Fork does:
   allocate static storage + stack + parameters, copy idata, apply irefs and
   load the initial registers into m68k. */
static uint32_t create_process_state(uint32_t mb, int pid, uint32_t params, uint32_t paramsz,
                                     uint32_t extra_mem, int npaths) {
    uint32_t execoff = RL(mb + 0x30);
    uint32_t memsize = RL(mb + 0x38), stacksize = RL(mb + 0x3C);
    uint32_t idata = RL(mb + 0x40), irefs = RL(mb + 0x44);
    uint32_t psz = (paramsz + 15) & ~15u;
    uint32_t total = memsize + stacksize + extra_mem + psz + 0x100;
    uint32_t data = mem_alloc(total, &total);
    if (!data) fatal("out of memory creating process");
    /* initialized data */
    uint32_t doff = RL(mb + idata), cnt = RL(mb + idata + 4);
    memcpy(RAM + data + doff, RAM + mb + idata + 8, cnt);
    /* irefs: two tables (code-relative, data-relative) */
    uint32_t o = mb + irefs;
    for (int t = 0; t < 2; t++) {
        for (;;) {
            uint32_t hi = RW(o), n = RW(o + 2);
            o += 4;
            if (!n) break;
            for (uint32_t k = 0; k < n; k++) {
                uint32_t off = (hi << 16) | RW(o);
                o += 2;
                WL(data + off, RL(data + off) + (t == 0 ? mb : data));
            }
        }
    }
    int64_t cyc = m68k.cyc, lim = m68k.cyc_limit;
    memset(&m68k, 0, sizeof m68k);
    m68k.cyc = cyc; m68k.cyc_limit = lim;
    uint32_t top = data + total;
    uint32_t pa = top - psz - 0x10;          /* parameter area at the top */
    for (uint32_t i = 0; i < paramsz; i++) WB(pa + i, RB(params + i));
    D(0) = (uint32_t)pid;
    D(1) = 0;               /* group/user */
    D(2) = 128;             /* priority */
    D(3) = (uint32_t)npaths;
    D(5) = paramsz;
    D(6) = total;
    A(1) = top;
    A(3) = mb;
    A(5) = pa;
    A(6) = data + 0x8000;
    A(7) = A(5);
    m68k.pc = mb + execoff;
    return data;
}

void os9_load_main_module(const uint8_t *mod, uint32_t len) {
    memcpy(RAM + MODULE_BASE, mod, len);
    uint32_t mb = MODULE_BASE;
    char nm[32];
    read_cstr(mb + RL(mb + 12), nm, sizeof nm);
    mod_register(nm, mb, RL(mb + 4));
    uint32_t data = create_process_state(mb, 1, 0, 0, 0, 3);
    /* MPW-style programs: the startup code sets a5 = data + 8 + idata[1] */
    a5_expected = MAIN_USES_A5 ? data + 8 + RL(data + 4) : 0;
    LOG("main module '%s' at %06x, data %06x, a5=%06x, entry %06x", nm, mb, data, a5_expected, m68k.pc);
}

static void write_csd(void) {
    char hp[600];
    nvr_host_path("csd", hp, sizeof hp);
    FILE *f = fopen(hp, "wb");
    if (!f) return;
    /* Device Status Descriptors of a typical NTSC base-case player */
    static const char *dsd[] = {
        "0::RV=\"1.1\":", "1:/cd:SP#27:", "2:/ap:AD#27:", "3:/vid:PL#2:LI=\"525\":TV:",
        "4:/nvr:SZ#8:", "5:/ptr:CL=\"d\":", "6:CDDRIVE:SK#3:", "8:MONITOR:LI=\"525\":",
    };
    for (size_t i = 0; i < sizeof dsd / sizeof dsd[0]; i++) {
        fputs(dsd[i], f);
        fputc(13, f);
    }
    fclose(f);
}

void os9_init(void) {
    heap_init();
    write_csd();
    procs[0].used = 1;
    procs[0].pid = 1;
    cur = &procs[0];
    paths[0].type = P_CONS;
    paths[1].type = P_CONS;
    paths[2].type = P_CONS;
}

/* ------------------------------------------------------------ syscalls */
static uint32_t ret_err(uint32_t err, uint32_t next) {
    D(1) = (D(1) & 0xFFFF0000u) | (err & 0xFFFF);
    CF = 1;
    return next;
}
static uint32_t ret_ok(uint32_t next) { CF = 0; return next; }

#define CHECK_SIG_RETURN(next) do { \
    if (sig_pending_count && !sig_masked && icpt_pc) { m68k.pc = (next); sig_deliver(); return m68k.pc; } \
    return (next); } while (0)

#define E_PRCFUL 0xE5
static struct {
    int state;               /* 0 none, 1 forked (not yet run), 2 running */
    int pid;
    uint32_t mod;
    M68K regs;
    volatile int exited;
    uint32_t status;
} child;

static uint32_t io_getstt(int p, uint32_t next);
static uint32_t io_setstt(int p, uint32_t next);

static uint32_t stack_caller(void) {
    for (int i = 0; i < 24; i++) {
        uint32_t v = RL(A(7) + i * 2);
        if (v >= recomp_code_lo && v < recomp_code_hi && !(v & 1)) return v;
    }
    return 0;
}

uint32_t os9_syscall(uint32_t code, uint32_t next) {
    if (log_level >= 2) {
        uint32_t c = stack_caller();
        TRACE("os9 %02x d0=%08x d1=%08x d2=%08x a0=%06x from %06x caller %06x (%s)", code, D(0), D(1), D(2), A(0), next, c, func_name(c));
        if (log_level >= 3) {
            char bt[512]; int k = 0;
            for (int i = 0; i < 64 && k < 480; i++) {
                uint32_t v = RL(A(7) + i * 2);
                if (v >= recomp_code_lo && v < recomp_code_hi && !(v & 1)) k += sprintf(bt + k, " %06x", v);
            }
            TRACE("   bt:%s", bt);
        }
    }
    switch (code) {
    case 0x00: { /* F$Link */
        char nm[64]; read_cstr(A(0), nm, sizeof nm);
        ModEntry *m = mod_find(nm);
        if (!m) { LOG("F$Link '%s' failed", nm); return ret_err(E_MNF, next); }
        m->links++;
        A(1) = m->addr + RL(m->addr + 0x30);
        A(2) = m->addr;
        D(0) = RW(m->addr + 0x12) & 0xFFFF;
        D(1) = RW(m->addr + 0x14);
        LOG("F$Link '%s' -> %06x", nm, m->addr);
        return ret_ok(next);
    }
    case 0x01: { /* F$Load */
        char nm[64]; read_cstr(A(0), nm, sizeof nm);
        const char *base = strrchr(nm, '/');
        base = base ? base + 1 : nm;
        ModEntry *m = mod_load_from_disc(base);
        if (!m) { LOG("F$Load '%s' failed", nm); return ret_err(E_PNNF_, next); }
        A(1) = m->addr + RL(m->addr + 0x30);
        A(2) = m->addr;
        D(0) = RW(m->addr + 0x12) & 0xFFFF;
        D(1) = RW(m->addr + 0x14);
        LOG("F$Load '%s' -> %06x", nm, m->addr);
        return ret_ok(next);
    }
    case 0x02: /* F$UnLink */
        return ret_ok(next);
    case 0x03: { /* F$Fork: the child is created now and runs when the parent waits */
        char nm[64]; read_cstr(A(0), nm, sizeof nm);
        ModEntry *m = mod_find(nm);
        if (!m) m = mod_load_from_disc(nm);
        if (!m) { LOG("F$Fork '%s': module not found", nm); return ret_err(E_MNF, next); }
        uint32_t entry = m->addr + RL(m->addr + 0x30);
        if (entry < recomp_code_lo || entry >= recomp_code_hi)
            LOG("F$Fork '%s': module at %06x is not recompiled, it will be interpreted", nm, m->addr);
        if (child.state) return ret_err(E_PRCFUL, next);
        M68K parent = m68k;
        static int next_pid = 2;
        int pid = next_pid++;
        create_process_state(m->addr, pid, A(1), D(2), D(1), D(3) & 0xFFFF);
        child.regs = m68k;
        m68k = parent;
        child.state = 1;
        child.pid = pid;
        child.mod = m->addr;
        Proc *p = &procs[1];
        memset(p, 0, sizeof *p);
        p->used = 1;
        p->pid = pid;
        LOG("F$Fork '%s' -> pid %d", nm, pid);
        D(0) = (uint32_t)pid;
        while (RB(A(0)) > ' ') A(0)++;
        return ret_ok(next);
    }
    case 0x04: /* F$Wait: run the child to completion */
        if (child.state == 1) {
            M68K parent = m68k;
            Proc *pp = cur;
            cur = proc_by_pid(child.pid);
            int64_t cyc = m68k.cyc, lim = m68k.cyc_limit;
            m68k = child.regs;
            m68k.cyc = cyc; m68k.cyc_limit = lim;
            child.state = 2;
            LOG("running child pid %d", child.pid);
            cpu_run_until(&child.exited);
            cyc = m68k.cyc; lim = m68k.cyc_limit;
            m68k = parent;
            m68k.cyc = cyc; m68k.cyc_limit = lim;
            cur = pp;
            proc_by_pid(child.pid)->used = 0;
            child.state = 0;
            child.exited = 0;
            LOG("child pid %d exited (%u)", child.pid, child.status);
            D(0) = (uint32_t)child.pid;
            D(1) = child.status;
            return ret_ok(next);
        }
        return ret_err(E_NOCHLD, next);
    case 0x05: /* F$Chain */
        fatal("F$Chain not supported");
        return next;
    case 0x06: /* F$Exit */
        if (cur && cur->pid != 1) {
            child.status = D(1) & 0xFFFF;
            child.exited = 1;
            return 0xFFFFFFFEu;       /* unwinds the child's native call chain */
        }
        LOG("F$Exit(%u)", D(1));
        exit((int)(D(1) & 0xFFFF));
        return next;
    case 0x1D: /* F$UnLoad */
        return ret_ok(next);
    case 0x21: /* F$TLink: trap handler modules are not needed by the recompiled code */
        return ret_ok(next);
    case 0x27: { /* F$SetSys: read/write a system global */
        uint32_t off = D(0) & 0xFFFF, sz = D(1);
        if (sz & 0x80000000u) {
            /* read: D_TckSec (ticks per second) and D_Ticks are what programs ask for */
            D(2) = off == 0x0E ? TICKS_PER_SEC : (uint32_t)(m68k.cyc / TICK_CYC);
            TRACE("F$SetSys read %x -> %u", off, D(2));
        } else {
            TRACE("F$SetSys write %x = %x (ignored)", off, D(2));
        }
        return ret_ok(next);
    }
    case 0x07: /* F$Mem */
        D(0) = 0;
        A(1) = 0;
        return ret_ok(next);
    case 0x08: { /* F$Send */
        int pid = D(0) & 0xFFFF;
        if (pid == 0) pid = os9_cur_pid();
        if (!proc_by_pid(pid)) return ret_err(E_IPRCID, next);
        sig_send_pid(pid, D(1) & 0xFFFF);
        CHECK_SIG_RETURN(ret_ok(next));
    }
    case 0x09: /* F$Icpt */
        icpt_pc = A(0);
        icpt_a6 = A(6);
        LOG("F$Icpt handler %06x a6=%06x", icpt_pc, icpt_a6);
        return ret_ok(next);
    case 0x0A: { /* F$Sleep (clears the signal mask, as OS-9 does) */
        /* Restartable: when a field completes while sleeping, return to the
           trap instruction itself so the CPU loop reaches the frame boundary
           with a flat host stack (needed for run-ahead snapshots); the trap
           then re-executes and continues the same sleep. */
        static int sleeping;
        static uint32_t sleep_pc;
        static int64_t sleep_until;
        uint32_t ticks = D(0);
        sig_masked = 0;
        if (!sleeping || sleep_pc != next) {
            sleeping = 1;
            sleep_pc = next;
            sleep_until = ticks ? m68k.cyc + (int64_t)ticks * TICK_CYC : INT64_MAX;
        }
        while (m68k.cyc < sleep_until) {
            if (sig_pending_count && !sig_masked && icpt_pc) {
                sleeping = 0;
                D(0) = 0;
                CF = 0;
                m68k.pc = next;
                sig_deliver();
                return m68k.pc;
            }
            sched_wait_event();
            if (sleep_until != INT64_MAX && m68k.cyc > sleep_until) break;
            if (g_frame_ready) return next - 4;
        }
        sleeping = 0;
        D(0) = 0;
        return ret_ok(next);
    }
    case 0x0C: /* F$ID */
        D(0) = 1;
        D(1) = 0;
        return ret_ok(next);
    case 0x0D: /* F$SPrior */
        return ret_ok(next);
    case 0x15: { /* F$Time */
        extern int g_headless;
        /* headless runs use a fixed epoch so test runs are reproducible */
        time_t t = g_headless ? (time_t)(820454400 + m68k.cyc / CPU_IPS) : time(NULL);
        struct tm *tm = localtime(&t);
        D(0) = ((uint32_t)tm->tm_hour << 16) | (tm->tm_min << 8) | tm->tm_sec;
        D(1) = ((uint32_t)(tm->tm_year + 1900) << 16) | ((tm->tm_mon + 1) << 8) | tm->tm_mday;
        D(2) = (TICKS_PER_SEC << 16) | (uint32_t)((m68k.cyc / TICK_CYC) % TICKS_PER_SEC);
        D(3) = (uint32_t)(m68k.cyc / TICK_CYC);
        return ret_ok(next);
    }
    case 0x18: { /* F$GPrDsc: d0 = pid, d1 = size, a0 = buffer */
        /* a copy of the process descriptor.  Programs read P$ID and the
           CPU-time counters (C library clock(): P$UTicks + P$STicks) */
        uint32_t n = D(1) > 0x800 ? 0x800 : D(1);
        for (uint32_t i = 0; i < n; i++) WB(A(0) + i, 0);
        uint32_t pid = D(0) ? D(0) : (uint32_t)cur->pid;
        if (n >= 2) WW(A(0) + 0x00, pid);                       /* P$ID */
        if (n >= 0x2BC) {
            WL(A(0) + 0x2B4, (uint32_t)(m68k.cyc / TICK_CYC));  /* P$UTicks */
            WL(A(0) + 0x2B8, 0);                                /* P$STicks */
        }
        return ret_ok(next);
    }
    case 0x1E: /* F$RTE */
        return do_rte();
    case 0x28: { /* F$SRqMem */
        uint32_t sz = D(0), act;
        if (sz == 0xFFFFFFFFu) sz = 0x40000;    /* "largest block" request */
        uint32_t a = mem_alloc(sz, &act);
        if (!a) return ret_err(E_MEMFUL, next);
        D(0) = act;
        A(2) = a;
        TRACE("F$SRqMem %x -> %06x", sz, a);
        return ret_ok(next);
    }
    case 0x29: /* F$SRtMem */
        mem_free(A(2), D(0));
        return ret_ok(next);
    case 0x56: { /* F$Alarm */
        uint32_t fn = D(1) & 0xFFFF;
        if (fn == 0) { /* A$Delete */
            for (int i = 0; i < 16; i++)
                if (alarms[i].used && (D(0) == 0 || alarms[i].id == D(0))) alarms[i].used = 0;
            return ret_ok(next);
        }
        int slot = -1;
        for (int i = 0; i < 16; i++) if (!alarms[i].used) { slot = i; break; }
        if (slot < 0) return ret_err(E_MEMFUL, next);
        Alarm *al = &alarms[slot];
        al->used = 1;
        al->pid = os9_cur_pid();
        al->id = next_alarm_id++;
        al->sig = D(2) & 0xFFFF;
        uint32_t t = D(3);
        int64_t cyc;
        if (t & 0x80000000u) cyc = (int64_t)(t & 0x7FFFFFFF) * CPU_IPS / 256; /* seconds/256 */
        else cyc = (int64_t)t * TICK_CYC;
        if (cyc < 1) cyc = 1;
        al->when = m68k.cyc + cyc;
        al->period = (fn == 2) ? cyc : 0;   /* A$Cycle */
        if (al->when < m68k.cyc_limit) m68k.cyc_limit = al->when;
        D(0) = al->id;
        TRACE("F$Alarm fn %u sig %u t %x -> id %u", fn, al->sig, t, al->id);
        return ret_ok(next);
    }
    case 0x57: /* F$SigMask */
        switch (D(1) & 0xFFFF) {
        case 0: sig_masked = 0; break;
        case 1: sig_masked++; break;
        case 0xFFFF: if (sig_masked) sig_masked--; break;
        default: sig_masked = (int)(D(1) & 0xFFFF);
        }
        CHECK_SIG_RETURN(ret_ok(next));
    case 0x5C: { /* F$SRqCMem: colored memory request */
        uint32_t sz = D(0), act;
        /* "largest block" request: a base-case player has two 512 KB video
           banks; report what is left of one after the system's share.
           Voyeur sizes its BOLT pools this way (up to $76000 per bank) and
           its zoomed viewfinder videos fail to load with smaller pools. */
        if (sz == 0xFFFFFFFFu) sz = 0x7C000;
        uint32_t a = mem_alloc(sz, &act);
        if (!a) return ret_err(E_MEMFUL, next);
        D(0) = act;
        A(2) = a;
        TRACE("F$SRqCMem %x type %x -> %06x", sz, D(1), a);
        return ret_ok(next);
    }

    /* ---- I/O */
    case 0x83: case 0x84: { /* I$Create / I$Open */
        char nm[128]; read_cstr(A(0), nm, sizeof nm);
        int r = do_open(nm, D(0) & 0xFF, code == 0x83);
        LOG("I$%s '%s' mode %x -> %d", code == 0x83 ? "Create" : "Open", nm, D(0) & 0xFF, r);
        if (r < 0) return ret_err((uint32_t)-r, next);
        uint32_t a = A(0);
        while (RB(a) > ' ') a++;
        A(0) = a;
        D(0) = (uint32_t)r;
        return ret_ok(next);
    }
    case 0x87: { /* I$Delete */
        char nm[128]; read_cstr(A(0), nm, sizeof nm);
        LOG("I$Delete '%s'", nm);
        if (!_strnicmp(nm, "/nvr/", 5)) {
            char hp[600]; nvr_host_path(nm + 5, hp, sizeof hp);
            remove(hp);
            return ret_ok(next);
        }
        return ret_err(E_PNNF_, next);
    }
    case 0x82: { /* I$Dup */
        int p = path_alloc();
        if (p < 0) return ret_err(E_PTHFUL, next);
        paths[p] = paths[D(0) & 0xFFFF];
        D(0) = (uint32_t)p;
        return ret_ok(next);
    }
    case 0x8F: { /* I$Close */
        int p = D(0) & 0xFFFF;
        if (p >= MAXPATH || paths[p].type == P_NONE) return ret_err(E_BPNUM_, next);
        Path *P = &paths[p];
        if (P->type == P_CD) cdfm_close(P->handle);
        if (p >= 3) memset(P, 0, sizeof *P);
        return ret_ok(next);
    }
    case 0x88: { /* I$Seek */
        int p = D(0) & 0xFFFF;
        if (p >= MAXPATH) return ret_err(E_BPNUM_, next);
        Path *P = &paths[p];
        if (P->type == P_CD) { cdfm_seek(P->handle, D(1)); return ret_ok(next); }
        if (P->type == P_NVR && P->host[0]) { P->pos = D(1); return ret_ok(next); }
        return ret_ok(next);
    }
    case 0x89: case 0x8B: { /* I$Read / I$ReadLn */
        int p = D(0) & 0xFFFF;
        if (p >= MAXPATH || paths[p].type == P_NONE) return ret_err(E_BPNUM_, next);
        Path *P = &paths[p];
        uint32_t n = D(1), got = 0;
        if (P->type == P_CD) {
            int e = cdfm_read(P->handle, A(0), n, &got);
            if (e) return ret_err((uint32_t)e, next);
            D(1) = got;
            return ret_ok(next);
        }
        if (P->type == P_NVR && P->host[0]) {
            FILE *fp = fopen(P->host, "rb");
            if (fp) {
                fseek(fp, (long)P->pos, SEEK_SET);
                for (got = 0; got < n; got++) {
                    int c = fgetc(fp);
                    if (c == EOF) break;
                    WB(A(0) + got, (uint32_t)c);
                    if (code == 0x8B && c == '\r') { got++; break; }
                }
                fclose(fp);
            }
            P->pos += got;
            if (!got && n) return ret_err(E_EOF_, next);
            D(1) = got;
            return ret_ok(next);
        }
        return ret_err(E_EOF_, next);
    }
    case 0x8A: case 0x8C: { /* I$Write / I$WritLn */
        int p = D(0) & 0xFFFF;
        if (p >= MAXPATH || paths[p].type == P_NONE) return ret_err(E_BPNUM_, next);
        Path *P = &paths[p];
        uint32_t n = D(1);
        if (P->type == P_NVR && P->host[0]) {
            uint32_t k = 0;
            for (; k < n; k++)
                if (code == 0x8C && RB(A(0) + k) == '\r') { k++; break; }
            if (!g_speculative) {      /* writes from run-ahead frames are redone for real */
                FILE *fp = fopen(P->host, "r+b");
                if (!fp) fp = fopen(P->host, "w+b");
                if (fp) {
                    fseek(fp, (long)P->pos, SEEK_SET);
                    for (uint32_t i = 0; i < k; i++) fputc(RB(A(0) + i), fp);
                    fclose(fp);
                }
            }
            P->pos += k;
            D(1) = k;
            return ret_ok(next);
        }
        if (P->type == P_CONS) {
            char buf[512];
            uint32_t k = 0;
            for (; k < n && k < sizeof buf - 1; k++) {
                char c = (char)RB(A(0) + k);
                buf[k] = c == '\r' ? '\n' : c;
                if (code == 0x8C && c == '\r') { k++; break; }
            }
            buf[k] = 0;
            LOG("[console] %s", buf);
            D(1) = k;
            return ret_ok(next);
        }
        D(1) = n;
        return ret_ok(next);
    }
    case 0x8D: return io_getstt(D(0) & 0xFFFF, next);
    case 0x8E: return io_setstt(D(0) & 0xFFFF, next);
    }
    {
        static uint8_t warned[256];
        if (!warned[code & 0xFF]) {
            warned[code & 0xFF] = 1;
            LOG("unimplemented OS-9 call $%02x at %06x (%s): returning E$UnkSvc", code, next - 4, func_name(next));
        }
    }
    return ret_err(E_UNKSVC_, next);
}

static uint32_t nvr_file_size(const char *hp) {
    struct stat st;
    return stat(hp, &st) ? 0 : (uint32_t)st.st_size;
}

static uint32_t io_getstt(int p, uint32_t next) {
    if (p >= MAXPATH || paths[p].type == P_NONE) return ret_err(E_BPNUM_, next);
    Path *P = &paths[p];
    uint32_t fn = D(1) & 0xFFFF;
    int e;
    switch (P->type) {
    case P_CD:
        if (fn == 2) { D(2) = cdfm_size(P->handle); return ret_ok(next); }   /* SS_Size */
        if (fn == 5) { D(2) = cdfm_getpos(P->handle); return ret_ok(next); } /* SS_Pos */
        if (fn == 6) { /* SS_EOF */
            if (cdfm_getpos(P->handle) >= cdfm_size(P->handle)) return ret_err(E_EOF_, next);
            D(1) = 0; return ret_ok(next);
        }
        e = cdfm_getstat(P->handle, fn);
        return e ? ret_err((uint32_t)e, next) : ret_ok(next);
    case P_NVR:
        if (fn == 2 && P->host[0]) { D(2) = nvr_file_size(P->host); return ret_ok(next); }
        if (fn == 5 && P->host[0]) { D(2) = P->pos; return ret_ok(next); }
        if (fn == 6 && P->host[0]) {
            if (P->pos >= nvr_file_size(P->host)) return ret_err(E_EOF_, next);
            return ret_ok(next);
        }
        if (fn == 0) { /* SS_Opt */
            for (int i = 0; i < 128; i++) WB(A(0) + i, 0);
            return ret_ok(next);
        }
        if (fn == 0x43) { /* NVRAM free space */
            D(2) = nvr_free_bytes();
            return ret_ok(next);
        }
        if (fn == 0x0F && P->host[0]) { /* SS_FD: RBF file descriptor, a0 = buffer, d2 = size */
            /* programs read the modification date (Voyeur shows "last play"
               from it); headless runs use the fixed test epoch */
            extern int g_headless;
            uint8_t fd[16] = {0};
            struct stat st;
            time_t t = 820454400;
            long size = 0;
            if (!stat(P->host, &st)) { size = (long)st.st_size; if (!g_headless) t = st.st_mtime; }
            struct tm *tm = localtime(&t);
            fd[0] = 0x3F;                                   /* FD_ATT: rwe for all */
            fd[3] = (uint8_t)tm->tm_year; fd[4] = (uint8_t)(tm->tm_mon + 1); fd[5] = (uint8_t)tm->tm_mday;
            fd[6] = (uint8_t)tm->tm_hour; fd[7] = (uint8_t)tm->tm_min;   /* FD_DAT */
            fd[8] = 1;                                      /* FD_LNK */
            fd[9] = (uint8_t)(size >> 24); fd[10] = (uint8_t)(size >> 16);
            fd[11] = (uint8_t)(size >> 8); fd[12] = (uint8_t)size;       /* FD_SIZ */
            fd[13] = fd[3]; fd[14] = fd[4]; fd[15] = fd[5];               /* FD_Creat */
            uint32_t n = D(2) & 0xFFFF;
            for (uint32_t i = 0; i < n; i++) WB(A(0) + i, i < sizeof fd ? fd[i] : 0);
            return ret_ok(next);
        }
        LOG("GetStt %x on nvr path", fn);
        return ret_err(E_UNKSVC_, next);
    case P_VIDEO:
        e = ucm_getstat(fn);
        return e ? ret_err((uint32_t)e, next) : ret_ok(next);
    case P_PTR:
        e = ptr_getstat(fn);
        return e ? ret_err((uint32_t)e, next) : ret_ok(next);
    case P_AUDIO:
        e = audio_getstat(fn);
        return e ? ret_err((uint32_t)e, next) : ret_ok(next);
    case P_CONS: case P_NULL: case P_KB:
        if (fn == 0) { for (int i = 0; i < 128; i++) WB(A(0) + i, 0); return ret_ok(next); }
        if (fn == 1) return ret_err(0xF6, next); /* SS_Ready: not ready */
        return ret_err(E_UNKSVC_, next);
    }
    return ret_err(E_UNKSVC_, next);
}

static uint32_t io_setstt(int p, uint32_t next) {
    if (p >= MAXPATH || paths[p].type == P_NONE) return ret_err(E_BPNUM_, next);
    Path *P = &paths[p];
    uint32_t fn = D(1) & 0xFFFF;
    int e;
    switch (P->type) {
    case P_CD:
        e = cdfm_setstat(P->handle, fn);
        if (e) return ret_err((uint32_t)e, next);
        CHECK_SIG_RETURN(ret_ok(next));
    case P_VIDEO:
        e = ucm_setstat(fn);
        return e ? ret_err((uint32_t)e, next) : ret_ok(next);
    case P_PTR:
        e = ptr_setstat(fn);
        return e ? ret_err((uint32_t)e, next) : ret_ok(next);
    case P_AUDIO:
        e = audio_setstat(fn);
        if (e) return ret_err((uint32_t)e, next);
        CHECK_SIG_RETURN(ret_ok(next));
    case P_NVR:
        if (fn == 2) return ret_ok(next);   /* SS_Size */
        if (fn == 0) return ret_ok(next);
        LOG("SetStt %x on nvr path", fn);
        return ret_ok(next);
    default:
        return ret_ok(next);
    }
}
