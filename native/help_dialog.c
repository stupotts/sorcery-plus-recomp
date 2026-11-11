#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include "app_icon.h"
#include "embedded_assets.h"
#include "help_dialog.h"
#include "resource.h"

#define LAUNCH_GUIDE_TITLE L"Sorcery+ Launch Guide"

static HWND add_control(HWND dialog, const wchar_t *class_name, const wchar_t *text,
                        DWORD extended_style, DWORD style, int id, RECT units)
{
    MapDialogRect(dialog, &units);
    HWND control = CreateWindowExW(extended_style, class_name, text, WS_CHILD | WS_VISIBLE | style,
                                   units.left, units.top, units.right - units.left,
                                   units.bottom - units.top, dialog, (HMENU)(INT_PTR)id,
                                   GetModuleHandleW(NULL), NULL);
    if (control) {
        SendMessageW(control, WM_SETFONT, SendMessageW(dialog, WM_GETFONT, 0, 0), TRUE);
    }
    return control;
}

static INT_PTR CALLBACK dialog_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_INITDIALOG: {
        app_icon_set(dialog);
        HWND guide = add_control(dialog, L"EDIT", (const wchar_t *)lparam, WS_EX_CLIENTEDGE,
            ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL | WS_TABSTOP,
            100, (RECT){8, 8, 382, 190});
        HWND close = add_control(dialog, L"BUTTON", L"Close", 0, BS_DEFPUSHBUTTON | WS_TABSTOP,
                                 IDOK, (RECT){331, 197, 382, 215});
        if (!guide || !close) {
            EndDialog(dialog, -1);
            return TRUE;
        }
        SendMessageW(guide, EM_SETSEL, 0, 0);
        SetFocus(guide);
        return FALSE;
    }
    case WM_CLOSE:
        EndDialog(dialog, IDOK);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wparam) == IDOK || LOWORD(wparam) == IDCANCEL) {
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

bool help_dialog_show(HWND owner)
{
    const char *guide = embedded_launch_guide(NULL);
    if (!guide || !*guide) return false;
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, guide, -1, NULL, 0);
    if (length <= 0) return false;
    wchar_t *text = malloc((size_t)length * sizeof(*text));
    if (!text) return false;
    bool shown = false;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, guide, -1, text, length)) {
        _Alignas(DWORD) static const struct {
            DLGTEMPLATE header;
            WORD no_menu, default_class;
            wchar_t title[ARRAYSIZE(LAUNCH_GUIDE_TITLE)];
            WORD font_size;
            wchar_t font_face[ARRAYSIZE(L"Consolas")];
        } layout = {
            .header = {
                .style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_CENTER | DS_SETFONT,
                .cx = 390, .cy = 223
            },
            .title = LAUNCH_GUIDE_TITLE, .font_size = 10, .font_face = L"Consolas"
        };
        shown = DialogBoxIndirectParamW(GetModuleHandleW(NULL), &layout.header, owner, dialog_proc,
                                        (LPARAM)text) == IDOK;
    }
    free(text);
    return shown;
}

static void draw_about_art(const DRAWITEMSTRUCT *draw, HBITMAP art)
{
    BITMAP bitmap;
    if (!GetObjectW(art, sizeof bitmap, &bitmap) || bitmap.bmWidth <= 0 || bitmap.bmHeight <= 0) return;
    HDC source = CreateCompatibleDC(draw->hDC);
    if (!source) return;
    HGDIOBJ previous = SelectObject(source, art);
    int available_width = draw->rcItem.right - draw->rcItem.left;
    int available_height = draw->rcItem.bottom - draw->rcItem.top;
    int height = available_height;
    int width = MulDiv(height, bitmap.bmWidth, bitmap.bmHeight);
    if (width > available_width) {
        width = available_width;
        height = MulDiv(width, bitmap.bmHeight, bitmap.bmWidth);
    }
    FillRect(draw->hDC, &draw->rcItem, GetSysColorBrush(COLOR_BTNFACE));
    SetStretchBltMode(draw->hDC, HALFTONE);
    SetBrushOrgEx(draw->hDC, 0, 0, NULL);
    StretchBlt(draw->hDC, draw->rcItem.left + (available_width - width) / 2,
               draw->rcItem.top + (available_height - height) / 2, width, height,
               source, 0, 0, bitmap.bmWidth, bitmap.bmHeight, SRCCOPY);
    SelectObject(source, previous);
    DeleteDC(source);
}

static INT_PTR CALLBACK about_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_INITDIALOG:
        app_icon_set(dialog);
        SetWindowLongPtrW(dialog, DWLP_USER, lparam);
        SetDlgItemTextW(dialog, IDC_ABOUT_VERSION, SORCERY_VERSION_LABEL);
        return TRUE;
    case WM_DRAWITEM:
        if (wparam == IDC_ABOUT_ART) {
            draw_about_art((const DRAWITEMSTRUCT *)lparam, (HBITMAP)GetWindowLongPtrW(dialog, DWLP_USER));
            return TRUE;
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wparam) != IDOK && LOWORD(wparam) != IDCANCEL) break;
        /* Fall through. */
    case WM_CLOSE:
        EndDialog(dialog, IDOK);
        return TRUE;
    }
    return FALSE;
}

bool about_dialog_show(HWND owner)
{
    HINSTANCE instance = GetModuleHandleW(NULL);
    HBITMAP art = LoadImageW(instance, MAKEINTRESOURCEW(IDB_ABOUT_ART), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
    if (!art) return false;
    bool shown = DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_ABOUT), owner, about_proc, (LPARAM)art) == IDOK;
    DeleteObject(art);
    return shown;
}
