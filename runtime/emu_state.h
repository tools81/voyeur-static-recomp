/* Machine state for save states / run-ahead.

   Every global and static variable of the emulated machine (CPU, OS-9
   kernel, CDFM, audio, video) is placed in two dedicated sections by
   including this header at the top of each emulation source file:
   .emud (initialized data) and .emub (zero-initialized data).
   savestate.c snapshots those sections as a whole, plus emulated RAM and the
   machine heap below, using the Win32 write watch to copy only pages that
   changed.  Host-side state (window, renderer, input, logging) lives in
   ordinary sections and is never rolled back.

   Machine-side heap memory must come from emu_alloc/emu_free. */
#pragma once
#include <stddef.h>

#if defined(_MSC_VER) && !defined(EMU_HOST_SIDE)
#pragma data_seg(".emud")
#pragma bss_seg(".emub")
#endif

void *emu_alloc(size_t n);          /* zeroed */
void emu_free(void *p);

extern int g_speculative;           /* running ahead: no host side effects (audio, files, log) */
