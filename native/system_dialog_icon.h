#ifndef SORCERY_SYSTEM_DIALOG_ICON_H
#define SORCERY_SYSTEM_DIALOG_ICON_H

#include <windows.h>

typedef struct AppDialogIconScope {
    HHOOK hook;
    HWND owner;
    struct AppDialogIconScope *previous;
} AppDialogIconScope;

void app_dialog_icon_begin(AppDialogIconScope *scope, HWND owner);
void app_dialog_icon_end(AppDialogIconScope *scope);
int app_message_box(HWND owner, const wchar_t *message, const wchar_t *title, UINT type);

#endif
