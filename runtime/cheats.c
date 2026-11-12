/* Voyeur cheats (F5: unlimited camera battery).

   The camcorder battery is the bar beside the viewfinder.  It is the clock of
   the current time period: the game's data area (a6 = $148000) holds

     a6-32638 ($140082)  seconds of camera use so far (the clock interrupt,
                         MainVoyeurIntFunc, adds 1 per second while the camera
                         is on; IncrementTime adds 1-2 for some actions)
     a6-31500 ($1404F4)  battery size for this period (105, 80, ...)
     a6-31502 ($1404F2)  period number

   When the used count reaches the size the period ends (the game moves on to
   the next one), and with fewer than 3 seconds left doInterface runs the
   "battery dying" countdown, which waits for the clock to reach the end.

   With the cheat on, the used count is held where it was (and never closer
   than 5 seconds to the end, so neither of those can start).  Because the
   game schedules the events in the building against the same clock, they
   wait too: the scene in each window stays as it is until the cheat is
   turned off. */
#include "recomp.h"
#include "runtime.h"

#define VOY_BATT_USED 0x140082
#define VOY_BATT_SIZE 0x1404F4
#define VOY_PERIOD    0x1404F2

int g_cheat_battery;

void cheats_field(void) {
    static int held = -1, period = -1;
    if (!g_cheat_battery) { held = -1; return; }
    int used = (int16_t)RW(VOY_BATT_USED), size = (int16_t)RW(VOY_BATT_SIZE), per = RW(VOY_PERIOD);
    if (size <= 5) { held = -1; return; }          /* no battery (title sequence) */
    if (held < 0 || per != period || used < held) { held = used; period = per; }   /* new period: follow the game */
    if (held > size - 5) held = size - 5;
    if (held < 0) held = 0;
    if (used != held) WW(VOY_BATT_USED, (uint16_t)held);
}
