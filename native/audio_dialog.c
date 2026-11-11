#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <strsafe.h>
#include "app_icon.h"
#include "audio_dialog.h"

#define AUDIO_DIALOG_TITLE L"Audio Settings"

enum {
    SFX_ENABLED = 100,
    SFX_VOLUME,
    SFX_PERCENT,
    MUSIC_ENABLED,
    MUSIC_VOLUME,
    MUSIC_PERCENT
};

static HWND add_control(HWND dialog, const wchar_t *class_name, const wchar_t *text,
                        DWORD style, int id, int x, int y, int width, int height)
{
    RECT units = {x, y, x + width, y + height};
    MapDialogRect(dialog, &units);
    HWND control = CreateWindowExW(0, class_name, text, WS_CHILD | WS_VISIBLE | style,
                                   units.left, units.top, units.right - units.left,
                                   units.bottom - units.top, dialog, (HMENU)(INT_PTR)id,
                                   GetModuleHandleW(NULL), NULL);
    if (control) {
        SendMessageW(control, WM_SETFONT, SendMessageW(dialog, WM_GETFONT, 0, 0), FALSE);
    }
    return control;
}

static void refresh_track(HWND dialog, int check_id, int track_id, int percent_id)
{
    bool enabled = check_id == MUSIC_ENABLED || IsDlgButtonChecked(dialog, check_id) == BST_CHECKED;
    unsigned volume = (unsigned)SendDlgItemMessageW(dialog, track_id, TBM_GETPOS, 0, 0);
    wchar_t text[8];
    if (SUCCEEDED(StringCchPrintfW(text, ARRAYSIZE(text), L"%u%%", volume))) {
        SetDlgItemTextW(dialog, percent_id, text);
    }
    EnableWindow(GetDlgItem(dialog, track_id), enabled);
    EnableWindow(GetDlgItem(dialog, percent_id), enabled);
}

static void set_track(HWND dialog, int check_id, bool enabled, int track_id,
                      unsigned volume, int percent_id)
{
    CheckDlgButton(dialog, check_id, enabled ? BST_CHECKED : BST_UNCHECKED);
    SendDlgItemMessageW(dialog, track_id, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendDlgItemMessageW(dialog, track_id, TBM_SETPAGESIZE, 0, 10);
    SendDlgItemMessageW(dialog, track_id, TBM_SETPOS, TRUE, volume);
    refresh_track(dialog, check_id, track_id, percent_id);
}

static bool populate_dialog(HWND dialog, const AudioSettings *settings)
{
    if (!add_control(dialog, L"BUTTON", L"Sound effects", BS_GROUPBOX, -1, 8, 6, 284, 45) ||
        !add_control(dialog, L"BUTTON", L"Enabled", BS_AUTOCHECKBOX | WS_TABSTOP, SFX_ENABLED, 16, 24, 58, 12) ||
        !add_control(dialog, TRACKBAR_CLASSW, L"", TBS_NOTICKS | WS_TABSTOP, SFX_VOLUME, 77, 17, 173, 24) ||
        !add_control(dialog, L"STATIC", L"", SS_RIGHT, SFX_PERCENT, 250, 24, 32, 12) ||
        !add_control(dialog, L"BUTTON", L"Music", BS_GROUPBOX, -1, 8, 56, 284, 58) ||
        !add_control(dialog, L"BUTTON", L"In gameplay", BS_AUTOCHECKBOX | WS_TABSTOP, MUSIC_ENABLED, 16, 74, 66, 12) ||
        !add_control(dialog, TRACKBAR_CLASSW, L"", TBS_NOTICKS | WS_TABSTOP, MUSIC_VOLUME, 85, 67, 165, 24) ||
        !add_control(dialog, L"STATIC", L"", SS_RIGHT, MUSIC_PERCENT, 250, 74, 32, 12) ||
        !add_control(dialog, L"STATIC", L"Music volume applies to the title screen and gameplay.", SS_LEFT, -1, 16, 95, 265, 12) ||
        !add_control(dialog, L"BUTTON", L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK, 187, 124, 48, 17) ||
        !add_control(dialog, L"BUTTON", L"Cancel", WS_TABSTOP, IDCANCEL, 244, 124, 48, 17)) {
        return false;
    }
    set_track(dialog, SFX_ENABLED, settings->sfx_enabled, SFX_VOLUME, settings->sfx_volume, SFX_PERCENT);
    set_track(dialog, MUSIC_ENABLED, settings->music_enabled, MUSIC_VOLUME, settings->music_volume, MUSIC_PERCENT);
    return true;
}

static INT_PTR CALLBACK dialog_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_INITDIALOG:
        app_icon_set(dialog);
        SetWindowLongPtrW(dialog, DWLP_USER, lparam);
        if (!populate_dialog(dialog, (const AudioSettings *)lparam)) {
            EndDialog(dialog, IDCANCEL);
        }
        return TRUE;
    case WM_CLOSE:
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    case WM_HSCROLL:
        if ((HWND)lparam == GetDlgItem(dialog, SFX_VOLUME)) {
            refresh_track(dialog, SFX_ENABLED, SFX_VOLUME, SFX_PERCENT);
            return TRUE;
        }
        if ((HWND)lparam == GetDlgItem(dialog, MUSIC_VOLUME)) {
            refresh_track(dialog, MUSIC_ENABLED, MUSIC_VOLUME, MUSIC_PERCENT);
            return TRUE;
        }
        break;
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case SFX_ENABLED:
            refresh_track(dialog, SFX_ENABLED, SFX_VOLUME, SFX_PERCENT);
            return TRUE;
        case MUSIC_ENABLED:
            refresh_track(dialog, MUSIC_ENABLED, MUSIC_VOLUME, MUSIC_PERCENT);
            return TRUE;
        case IDOK: {
            AudioSettings *settings = (AudioSettings *)GetWindowLongPtrW(dialog, DWLP_USER);
            settings->sfx_enabled = IsDlgButtonChecked(dialog, SFX_ENABLED) == BST_CHECKED;
            settings->music_enabled = IsDlgButtonChecked(dialog, MUSIC_ENABLED) == BST_CHECKED;
            settings->sfx_volume = (unsigned)SendDlgItemMessageW(dialog, SFX_VOLUME, TBM_GETPOS, 0, 0);
            settings->music_volume = (unsigned)SendDlgItemMessageW(dialog, MUSIC_VOLUME, TBM_GETPOS, 0, 0);
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

bool audio_dialog_show(HWND owner, AudioSettings *settings)
{
    INITCOMMONCONTROLSEX controls = {.dwSize = sizeof(controls), .dwICC = ICC_BAR_CLASSES};
    if (!InitCommonControlsEx(&controls)) return false;
    _Alignas(DWORD) static const struct {
        DLGTEMPLATE header;
        WORD no_menu, default_class;
        wchar_t title[ARRAYSIZE(AUDIO_DIALOG_TITLE)];
        WORD font_size;
        wchar_t font_face[ARRAYSIZE(L"Segoe UI")];
    } layout = {
        .header = {
            .style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_CENTER | DS_SETFONT,
            .cx = 300, .cy = 150
        },
        .title = AUDIO_DIALOG_TITLE, .font_size = 9, .font_face = L"Segoe UI"
    };
    return DialogBoxIndirectParamW(GetModuleHandleW(NULL), &layout.header, owner, dialog_proc,
                                   (LPARAM)settings) == IDOK;
}
