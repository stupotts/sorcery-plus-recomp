#ifndef SORCERY_PRESENTATION_H
#define SORCERY_PRESENTATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <windows.h>
#include "audio_settings.h"

enum {
    DISPLAY_ACTION_SAVE = 1,
    DISPLAY_ACTION_LOAD = 2,
    DISPLAY_ACTION_CLOSE = 4,
    DISPLAY_ACTION_EXIT_TO_TITLE = 8,
    DISPLAY_ACTION_AUDIO_SETTINGS = 16,
    DISPLAY_ACTION_RESET_HIGH_SCORES = 32
};

bool display_open(void);
void display_set_room_title(int room);
HWND display_owner(void);
bool display_pump(void);
void display_frame(const uint8_t *vram, const uint8_t *inks,
                   unsigned startAddress, int splitLine, int topMode, int bottomMode);
void display_frame_split(const uint8_t *vram, const uint8_t *topInks,
                         const uint8_t *bottomInks, unsigned startAddress,
                         int splitLine, int topMode, int bottomMode);
bool display_snapshot(const wchar_t *path);
uint8_t display_key_row(unsigned row);
unsigned display_take_action(void);
void display_get_audio_settings(AudioSettings *settings);
bool display_confirm(const wchar_t *message);
void display_notice(const wchar_t *message, bool error);
void display_close(void);

#endif
