#ifndef SORCERY_MUSIC_H
#define SORCERY_MUSIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool music_load(const uint8_t *decoded, size_t size);
void music_start(void);
void music_stop(void);
bool music_half_tick(void);

#endif
