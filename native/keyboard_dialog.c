#define WIN32_LEAN_AND_MEAN
#include "keyboard_dialog.h"
#include "app_icon.h"

#include <string.h>
#include <wchar.h>

enum { KEY_COUNT = 6, FIRST_KEY = 100, STATUS_TEXT = 106, RESTORE_DEFAULTS = 107 };

static const wchar_t *const action_names[KEY_COUNT] = {
    L"\x2191 Up", L"\x2193 Down", L"\x2190 Left", L"\x2192 Right", L"Fire", L"Pause"
};

#define KEYBOARD_DIALOG_TITLE L"Keyboard Settings"
static const wchar_t ready_status[] = L"Select a control, then press its new key.";

struct keyboard_dialog {
    unsigned keys[KEY_COUNT];
    const unsigned *defaults;
    WNDPROC button_proc[KEY_COUNT];
    int capturing;
    unsigned modifier;
    unsigned release_key;
};

static void key_name(unsigned key, wchar_t name[32])
{
    switch (key) {
    case VK_LEFT: wcscpy(name, L"Left Arrow"); return;
    case VK_RIGHT: wcscpy(name, L"Right Arrow"); return;
    case VK_UP: wcscpy(name, L"Up Arrow"); return;
    case VK_DOWN: wcscpy(name, L"Down Arrow"); return;
    case VK_SPACE: wcscpy(name, L"Space"); return;
    case VK_SHIFT: wcscpy(name, L"Shift"); return;
    case VK_CONTROL: wcscpy(name, L"Ctrl"); return;
    }

    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) {
        name[0] = (wchar_t)key;
        name[1] = 0;
        return;
    }
    const UINT scan = MapVirtualKeyW(key, MAPVK_VK_TO_VSC_EX);
    const LONG code = (LONG)((scan & 0xff) << 16) | ((scan & 0xff00) ? 1L << 24 : 0);
    if (!scan || !GetKeyNameTextW(code, name, 32)) {
        swprintf(name, 32, L"Key %02X", key);
    }
}

static void update_button(HWND dialog, const struct keyboard_dialog *state, int action)
{
    wchar_t name[32], text[64];
    key_name(state->keys[action], name);
    swprintf(text, ARRAYSIZE(text), L"%ls\n%ls", action_names[action],
             state->capturing == action ? L"Press a key" : name);
    SetDlgItemTextW(dialog, FIRST_KEY + action, text);
}

static void stop_capture(HWND dialog, struct keyboard_dialog *state)
{
    if (state->capturing < 0) {
        return;
    }
    const int action = state->capturing;
    state->capturing = -1;
    state->modifier = 0;
    update_button(dialog, state, action);
    SetDlgItemTextW(dialog, STATUS_TEXT, ready_status);
}

static void capture_key(HWND dialog, struct keyboard_dialog *state, unsigned key)
{
    if (key < 8 || key > 254 || key == VK_F5 || key == VK_F9 || key == VK_F10 ||
        key == VK_F11 || key == VK_MENU || key == VK_LWIN || key == VK_RWIN ||
        key == VK_APPS || key == VK_PROCESSKEY || key == VK_PACKET) {
        SetDlgItemTextW(dialog, STATUS_TEXT, L"That key is reserved. Choose another key.");
        return;
    }

    for (int action = 0; action < KEY_COUNT; ++action) {
        if (action != state->capturing && state->keys[action] == key) {
            SetDlgItemTextW(dialog, STATUS_TEXT, L"That key is already assigned. Choose another key.");
            return;
        }
    }
    state->keys[state->capturing] = key;
    stop_capture(dialog, state);
}

static LRESULT CALLBACK key_button(HWND button, UINT message, WPARAM wparam, LPARAM lparam)
{
    HWND dialog = GetParent(button);
    struct keyboard_dialog *state = (struct keyboard_dialog *)GetWindowLongPtrW(dialog, DWLP_USER);
    const int action = GetDlgCtrlID(button) - FIRST_KEY;
    const unsigned key = (unsigned)wparam;
    if (state->capturing == action) {
        switch (message) {
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS;
        case WM_KILLFOCUS:
            stop_capture(dialog, state);
            break;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (lparam & (1L << 30)) {
                return 0;
            }
            state->release_key = key;
            if (key == VK_ESCAPE) {
                stop_capture(dialog, state);
            } else if (message == WM_SYSKEYDOWN || state->modifier ||
                       (GetKeyState(VK_CONTROL) < 0 && key != VK_CONTROL) ||
                       (GetKeyState(VK_SHIFT) < 0 && key != VK_SHIFT)) {
                state->modifier = 0;
                SetDlgItemTextW(dialog, STATUS_TEXT, L"Use a single key for this control.");
            } else if (key == VK_SHIFT || key == VK_CONTROL) {
                // Bind a modifier only on release so a chord cannot bind accidentally.
                state->modifier = key;
            } else {
                capture_key(dialog, state, key);
            }
            return 0;
        case WM_KEYUP:
            if (key == state->modifier) {
                state->modifier = 0;
                state->release_key = 0;
                capture_key(dialog, state, key);
                return 0;
            }
            break;
        }
    }
    if ((message == WM_KEYUP || message == WM_SYSKEYUP) && key == state->release_key) {
        state->release_key = 0;
        return 0;
    }
    return CallWindowProcW(state->button_proc[action], button, message, wparam, lparam);
}

static HWND add_control(HWND dialog, const wchar_t *class_name, const wchar_t *text,
                        DWORD style, int id, int x, int y, int width, int height)
{
    RECT units = {x, y, x + width, y + height};
    if (!MapDialogRect(dialog, &units)) {
        return NULL;
    }
    HWND control = CreateWindowExW(0, class_name, text, WS_CHILD | WS_VISIBLE | style,
                                   units.left, units.top, units.right - units.left,
                                   units.bottom - units.top, dialog, (HMENU)(INT_PTR)id,
                                   GetModuleHandleW(NULL), NULL);
    if (control) {
        SendMessageW(control, WM_SETFONT, SendMessageW(dialog, WM_GETFONT, 0, 0), FALSE);
    }
    return control;
}

static bool populate_dialog(HWND dialog, struct keyboard_dialog *state)
{
    static const int positions[KEY_COUNT][2] = {
        {75, 19}, {75, 70}, {16, 44}, {134, 44}, {207, 29}, {207, 61}
    };
    if (!add_control(dialog, L"BUTTON", L"Joystick", BS_GROUPBOX, -1, 9, 6, 187, 92)) {
        return false;
    }
    for (int action = 0; action < KEY_COUNT; ++action) {
        HWND button = add_control(dialog, L"BUTTON", L"", WS_TABSTOP | BS_MULTILINE,
                                  FIRST_KEY + action, positions[action][0], positions[action][1],
                                  action < 4 ? 55 : 65, 22);
        if (!button) {
            return false;
        }
        state->button_proc[action] = (WNDPROC)SetWindowLongPtrW(button, GWLP_WNDPROC, (LONG_PTR)key_button);
        if (!state->button_proc[action]) {
            return false;
        }
        update_button(dialog, state, action);
    }
    return add_control(dialog, L"STATIC", ready_status, SS_LEFT, STATUS_TEXT, 10, 106, 262, 14) &&
           add_control(dialog, L"BUTTON", L"Restore Defaults", WS_TABSTOP, RESTORE_DEFAULTS, 10, 126, 84, 17) &&
           add_control(dialog, L"BUTTON", L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK, 171, 126, 47, 17) &&
           add_control(dialog, L"BUTTON", L"Cancel", WS_TABSTOP, IDCANCEL, 225, 126, 47, 17);
}

static INT_PTR CALLBACK dialog_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam)
{
    struct keyboard_dialog *state = (struct keyboard_dialog *)GetWindowLongPtrW(dialog, DWLP_USER);
    switch (message) {
    case WM_INITDIALOG:
        app_icon_set(dialog);
        state = (struct keyboard_dialog *)lparam;
        SetWindowLongPtrW(dialog, DWLP_USER, lparam);
        if (!populate_dialog(dialog, state)) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        SetFocus(GetDlgItem(dialog, FIRST_KEY));
        return FALSE;
    case WM_CLOSE:
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    case WM_COMMAND: {
        const int id = LOWORD(wparam);
        if (id == IDOK || id == IDCANCEL) {
            EndDialog(dialog, id);
            return TRUE;
        }
        if (id == RESTORE_DEFAULTS) {
            stop_capture(dialog, state);
            memcpy(state->keys, state->defaults, sizeof(state->keys));
            for (int action = 0; action < KEY_COUNT; ++action) {
                update_button(dialog, state, action);
            }
            SetDlgItemTextW(dialog, STATUS_TEXT, L"Defaults restored. Select OK to keep them.");
            return TRUE;
        }
        if (id >= FIRST_KEY && id < FIRST_KEY + KEY_COUNT) {
            stop_capture(dialog, state);
            state->capturing = id - FIRST_KEY;
            update_button(dialog, state, state->capturing);
            SetDlgItemTextW(dialog, STATUS_TEXT, L"Press a key. Esc cancels this binding change.");
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

bool keyboard_dialog_show(HWND owner, unsigned keys[KEY_COUNT], const unsigned defaults[KEY_COUNT])
{
    // Dialog templates are DWORD-aligned, with WORD fields after DLGTEMPLATE.
    _Alignas(DWORD) static const struct {
        DLGTEMPLATE header;
        WORD no_menu, default_class;
        wchar_t title[ARRAYSIZE(KEYBOARD_DIALOG_TITLE)];
        WORD font_size;
        wchar_t font_face[ARRAYSIZE(L"Segoe UI")];
    } layout = {
        .header = {.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_CENTER | DS_SETFONT,
                   .cx = 282, .cy = 151},
        .title = KEYBOARD_DIALOG_TITLE, .font_size = 9, .font_face = L"Segoe UI"
    };
    struct keyboard_dialog state = {.defaults = defaults, .capturing = -1};
    memcpy(state.keys, keys, sizeof(state.keys));
    if (DialogBoxIndirectParamW(GetModuleHandleW(NULL), &layout.header, owner, dialog_proc,
                                (LPARAM)&state) != IDOK) {
        return false;
    }
    memcpy(keys, state.keys, sizeof(state.keys));
    return true;
}
