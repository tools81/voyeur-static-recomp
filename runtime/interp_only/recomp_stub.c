/* Interpreter-only build (the public release): the game's program is read
   from the player's own disc and run by the 68000 interpreter (interp.c),
   so the executable contains none of the game's code.  The tables below are
   what tools/gen.py would otherwise generate into gen/recomp_table.c. */
#include "recomp.h"

const uint32_t recomp_code_lo = 0, recomp_code_hi = 0;
const RecompEntry recomp_entries[] = { {0, NULL} };
const int recomp_num_entries = 0;
const RecompName recomp_names[] = { {0, NULL} };
const int recomp_num_names = 0;
