#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "app_icon.h"
#include "system_dialog_icon.h"

static _Thread_local AppDialogIconScope *active_scope;

static bool is_system_dialog(HWND window, HWND owner)
{
    WCHAR class_name[8];
    LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    if (window == owner || (style & WS_CHILD) || (style & WS_CAPTION) != WS_CAPTION ||
        !GetClassNameW(window, class_name, ARRAYSIZE(class_name)) ||
        lstrcmpW(class_name, L"#32770")) return false;

    HWND current_owner = GetWindow(window, GW_OWNER);
    if (!owner) return !current_owner;
    while (current_owner && current_owner != owner) {
        current_owner = GetWindow(current_owner, GW_OWNER);
    }
    return current_owner == owner;
}

static LRESULT CALLBACK activate_dialog(int code, WPARAM wparam, LPARAM lparam)
{
    HWND window = (HWND)wparam;
    if (code == HCBT_ACTIVATE && active_scope && is_system_dialog(window, active_scope->owner)) {
        LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
        LONG_PTR extended_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
        const bool add_menu = !(style & WS_SYSMENU);
        if (add_menu) {
            /* The caption icon needs a system menu. Keep originally unavailable Close disabled. */
            SetWindowLongPtrW(window, GWL_STYLE, style | WS_SYSMENU);
            EnableMenuItem(GetSystemMenu(window, FALSE), SC_CLOSE, MF_BYCOMMAND | MF_GRAYED);
        }
        /* Windows omits the caption icon on a dialog with this frame style. */
        if (extended_style & WS_EX_DLGMODALFRAME) {
            SetWindowLongPtrW(window, GWL_EXSTYLE, extended_style & ~WS_EX_DLGMODALFRAME);
        }
        app_icon_set(window);
        if (add_menu || (extended_style & WS_EX_DLGMODALFRAME)) {
            SetWindowPos(window, NULL, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
    }
    return CallNextHookEx(NULL, code, wparam, lparam);
}

void app_dialog_icon_begin(AppDialogIconScope *scope, HWND owner)
{
    *scope = (AppDialogIconScope){ .owner = owner, .previous = active_scope };
    if (!active_scope) {
        scope->hook = SetWindowsHookExW(WH_CBT, activate_dialog, NULL, GetCurrentThreadId());
        if (!scope->hook) return;
    }
    active_scope = scope;
}

void app_dialog_icon_end(AppDialogIconScope *scope)
{
    if (active_scope == scope) active_scope = scope->previous;
    if (scope->hook) UnhookWindowsHookEx(scope->hook);
}

int app_message_box(HWND owner, const wchar_t *message, const wchar_t *title, UINT type)
{
    AppDialogIconScope scope;
    app_dialog_icon_begin(&scope, owner);
    int result = MessageBoxW(owner, message, title, type);
    app_dialog_icon_end(&scope);
    return result;
}
