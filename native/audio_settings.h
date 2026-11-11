#ifndef SORCERY_AUDIO_SETTINGS_H
#define SORCERY_AUDIO_SETTINGS_H

#include <stdbool.h>

typedef struct {
    bool sfx_enabled;
    bool music_enabled;
    unsigned sfx_volume;
    unsigned music_volume;
} AudioSettings;

#endif
