#ifndef SORCERY_ROOM_ATLAS_H
#define SORCERY_ROOM_ATLAS_H

#include <stdbool.h>
#include <stdint.h>
#include <windows.h>

bool room_atlas_open(HWND owner);
void room_atlas_update(const uint8_t *memory, const wchar_t *path);
void room_atlas_clear(void);

#endif
