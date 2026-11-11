#ifndef SORCERY_SOUND_MANAGER_H
#define SORCERY_SOUND_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

void sound_reset(void);
bool sound_queue(const uint8_t data[9]);
uint8_t sound_status(uint8_t mask);
void sound_envelope(bool tone, unsigned index, const uint8_t data[16]);

// Advances the firmware queues and produces one 20 ms output block.
void sound_frame(void);

#endif
