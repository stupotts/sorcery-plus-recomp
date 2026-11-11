#ifndef SORCERY_AUDIO_H
#define SORCERY_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

// Call from the game thread. If the audio device is unavailable, continue muted.
bool audio_open(void);
void audio_register(unsigned reg, uint8_t value);
void audio_set_levels(unsigned sfx, unsigned music, bool enable_gameplay_music);
void audio_set_title_music(bool active);
void audio_music_register(unsigned reg, uint8_t value);
void audio_music_reset(void);
void audio_half_tick(void);
void audio_tick(void);
void audio_close(void);
void audio_measure(void);
uint64_t audio_pcm_hash(void);
unsigned audio_submissions(void);

#endif
