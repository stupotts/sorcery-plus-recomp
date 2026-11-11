#ifndef SORCERY_SAVE_DIALOG_H
#define SORCERY_SAVE_DIALOG_H

#include <stdbool.h>
#include <stddef.h>
#include <windef.h>

typedef enum {
    SAVE_DIALOG_CANCELED,
    SAVE_DIALOG_SELECTED,
    SAVE_DIALOG_ERROR
} SaveDialogResult;

/* Leave path unchanged unless the user selects a .sav file. */
SaveDialogResult save_dialog_choose(HWND owner, const wchar_t *initial_directory,
                                   bool saving, wchar_t *path, size_t capacity);

#endif
