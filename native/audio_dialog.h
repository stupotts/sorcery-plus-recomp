#ifndef SORCERY_AUDIO_DIALOG_H
#define SORCERY_AUDIO_DIALOG_H

#include <windows.h>
#include "audio_settings.h"

bool audio_dialog_show(HWND owner, AudioSettings *settings);

#endif
