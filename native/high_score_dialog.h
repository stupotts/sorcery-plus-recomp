#ifndef SORCERY_HIGH_SCORE_DIALOG_H
#define SORCERY_HIGH_SCORE_DIALOG_H

#include <windows.h>
#include "high_scores.h"

bool high_score_dialog_show(HWND owner, const uint8_t *table, bool (*reset)(void));

#endif
