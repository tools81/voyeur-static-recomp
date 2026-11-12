/* Frame boundary: presentation, input and run-ahead.

   video_field() renders a field and sets g_frame_ready; the CPU loop calls
   frame_boundary() as soon as it is back at top level (translated code has
   returned to the dispatcher), so the whole machine state is in RAM, the
   machine heap and the .emud/.emub sections and can be snapshotted.

   Run-ahead (g_runahead = N): after each real field the state is saved and
   N more fields are emulated with the same input, without audio or file
   writes.  The last of those is shown, then the state is restored and the
   next real field runs with fresh input.  The game's own input latency is
   hidden: a button press shows its effect N fields sooner. */
#include "runtime.h"
#define EMU_HOST_SIDE   /* host code: ordinary sections */
#include "emu_state.h"
#include "savestate.h"

volatile int g_frame_ready;
VideoFieldInfo g_vfi;
int g_runahead;

static int spec_left, snap_depth;

static void present(void) {
    host_present(video_fb, video_w, video_h);
}

void frame_boundary(int depth) {
    g_frame_ready = 0;
    if (g_speculative) {
        if (--spec_left > 0) return;
        g_speculative = 0;        /* host side effects of presenting (log, dumps) are real */
        present();
        if (depth == snap_depth) {
            state_load();
        } else {
            /* the speculative fields crossed into or out of a child process
               (another CPU loop level): keep them as the real ones */
            static int warned;
            if (!warned++) LOG("run-ahead: process change during speculation, frames committed");
        }
        host_poll();
        return;
    }
    /* a real field: run ahead from here (only in the main program loop) */
    if (g_runahead > 0 && depth == 1 && state_available()) {
        state_save();
        snap_depth = depth;
        spec_left = g_runahead;
        g_speculative = 1;
        return;
    }
    present();
    host_poll();
}
