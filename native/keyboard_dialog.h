#ifndef SORCERY_KEYBOARD_DIALOG_H
#define SORCERY_KEYBOARD_DIALOG_H

#include <stdbool.h>
#include <windows.h>

// Up, Down, Left, Right, Fire and Pause. Commit changes only when OK is pressed.
bool keyboard_dialog_show(HWND owner, unsigned keys[6], const unsigned defaults[6]);

#endif
