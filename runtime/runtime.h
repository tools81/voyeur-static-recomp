/* Runtime-internal interfaces (HLE of CD-RTOS + host platform). */
#pragma once
#include <stdint.h>
#include <stdio.h>

/* ---- logging */
extern int log_level;
void logf_(int lvl, const char *fmt, ...);
#define LOG(...)   logf_(1, __VA_ARGS__)
#define TRACE(...) do { if (log_level >= 2) logf_(2, __VA_ARGS__); } while (0)
void fatal(const char *fmt, ...);
const char *func_name(uint32_t pc);

/* ---- cpu */
void cpu_init(void);
void cpu_run(void);
void cpu_run_until(volatile int *flag);

/* ---- timing / scheduler */
#define CPU_IPS        4000000      /* emulated instructions per second */
#define FIELD_RATE     60           /* NTSC */
#define SECTOR_RATE    75           /* 1x CD */
void sched_init(void);
void sched_service(void);           /* called when m68k.cyc >= cyc_limit */
void sched_wait_event(void);        /* fast-forward to the next scheduled event (blocking syscalls) */
int64_t sched_now(void);

/* ---- memory allocator (OS-9 F$SRqMem) */
#define HEAP_BASE 0x00140000u
#define HEAP_END  0x00F00000u
uint32_t mem_alloc(uint32_t size, uint32_t *actual);
void mem_free(uint32_t addr, uint32_t size);

/* ---- OS-9 process / signals */
void os9_init(void);
void os9_load_main_module(const uint8_t *mod, uint32_t len);
void sig_send(uint32_t sig);        /* queue a signal for the current process */
void sig_send_pid(int pid, uint32_t sig);
int  os9_cur_pid(void);
int  sig_deliver(void);             /* deliver a pending signal if possible: returns 1 if pc changed */

/* OS-9 error codes */
#define E_ILLFNC 0xD2   /* hmm placeholder */
#define E_UNKSVC 0xD0
#define E_BPNUM  0xC9
#define E_PNNF   0xD8
#define E_EOF    0xD3
#define E_NORAM  0xCF  /* E$MemFul is 0xCF? */
#define E_ILLPRM 0xBB   /* placeholder */
#define E_BMODE  0xCB
#define E_UNID   0xBC   /* placeholder */

/* ---- disc */
typedef struct {
    char name[32];
    uint32_t lsn;
    uint32_t size;
    uint8_t  fnum;
    uint16_t attr;
} DiscFile;

int  disc_open(const char *path);
int  disc_num_files(void);
const DiscFile *disc_find(const char *name);
const DiscFile *disc_file(int i);
void disc_read_raw(uint32_t lsn, uint8_t *buf2352);
int  disc_read_file(const DiscFile *f, uint8_t **out, uint32_t *len);

/* ---- CDFM */
void cdfm_init(void);
void cdfm_sector_tick(void);        /* one 1x sector period elapsed */
int  cdfm_open(const char *name, int mode);   /* returns handle or -1 */
void cdfm_close(int h);
int  cdfm_read(int h, uint32_t buf, uint32_t n, uint32_t *got);
int  cdfm_seek(int h, uint32_t pos);
uint32_t cdfm_getpos(int h);
uint32_t cdfm_size(int h);
int  cdfm_setstat(int h, uint32_t code);      /* uses m68k regs; returns error or 0 */
int  cdfm_getstat(int h, uint32_t code);
int  cdfm_busy(void);

/* ---- audio */
void audio_init(void);
int  audio_setstat(uint32_t code);          /* SS_SM / SS_SC / SS_SD on the audio path */
int  audio_getstat(uint32_t code);
void audio_feed_sector(const uint8_t *sector2352);   /* ADPCM sector to the audio processor */
void audio_field_tick(void);
void audio_shutdown(void);

/* ---- video (UCM) */
void video_init(void);
int  ucm_setstat(uint32_t code);
int  ucm_getstat(uint32_t code);
void video_field(void);             /* run display program for one field, render */
extern uint32_t *video_fb;          /* 32bpp host framebuffer */
extern int video_w, video_h;

/* ---- input */
typedef struct {
    int dx, dy;         /* relative motion this field (thumbpad) */
    int b1, b2, b3;
    int x, y;           /* absolute pointer position */
    int quit;
} InputState;
extern InputState input;
int  ptr_setstat(uint32_t code);
int  ptr_getstat(uint32_t code);

/* ---- per-field picture identity for the HD renderer (host-side, frame.c).
   For each plane, a hash of the coded bytes of every line shown in a
   photographic coding (RL7 video, DYUV); the HD pack is keyed by it. */
#define FNV64_INIT 0xcbf29ce484222325ull
static inline uint64_t fnv64(uint64_t h, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}
typedef struct {
    int lines[2];        /* lines decoded in RL7 or DYUV */
    int coding[2];       /* 5 DYUV, 0x47 RL7 (last line), 0 other */
    uint64_t hash[2];
    int cur_line;                    /* line being decoded */
    uint64_t line_hash[2][240];      /* per display line: hash of its coded bytes (0 = not photographic) */
} VideoFieldInfo;
extern VideoFieldInfo g_vfi;

/* ---- frame boundary (frame.c) */
extern volatile int g_frame_ready;  /* a field was rendered: present it at top level */
void frame_boundary(int depth);
extern int g_runahead;
/* cheats.c: game cheats, applied once per field */
extern int g_cheat_battery;
void cheats_field(void);              /* fields to run ahead (0 = off) */

/* ---- host platform */
int  host_init(void);
void host_present(const uint32_t *fb, int w, int h);
void host_poll(void);
double host_time(void);
void host_sleep_until(double t);
void host_audio_init(int rate);
void host_audio_push(const int16_t *stereo, int frames);
int  host_audio_queued(void);

/* paths */
extern char g_data_dir[512];
