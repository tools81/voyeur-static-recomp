/* In-memory machine snapshots (savestate.c). */
#pragma once
#include <stddef.h>
#include <stdint.h>

uint8_t *state_alloc_ram(size_t size);   /* emulated RAM, write-watched */
int  state_available(void);
void state_save(void);
void state_load(void);
