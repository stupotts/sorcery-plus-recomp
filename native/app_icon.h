#ifndef SORCERY_APP_ICON_H
#define SORCERY_APP_ICON_H

#include <stdbool.h>
#include <windows.h>

HICON app_icon(bool small);
void app_icon_set(HWND window);

#endif
