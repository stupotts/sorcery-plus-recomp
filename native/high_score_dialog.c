#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <wchar.h>
#include "app_icon.h"
#include "high_score_dialog.h"
#include "resource.h"
#include "system_dialog_icon.h"

typedef struct {
    const uint8_t *table;
    bool (*reset)(void);
} HighScoreDialog;

static void fill_scores(HWND list, const uint8_t *table)
{
    ListView_DeleteAllItems(list);
    for (unsigned rank = 0; rank < HIGH_SCORE_COUNT; ++rank) {
        HighScoreEntry entry = high_scores_entry(table, rank);
        wchar_t position[8], points[12];
        swprintf(position, ARRAYSIZE(position), L"%u", rank + 1);
        swprintf(points, ARRAYSIZE(points), L"%06u", entry.points);
        LVITEMW item = {.mask = LVIF_TEXT, .iItem = (int)rank, .pszText = position};
        int row = (int)SendMessageW(list, LVM_INSERTITEMW, 0, (LPARAM)&item);
        if (row < 0) return;
        item.iSubItem = 1;
        item.pszText = entry.name;
        SendMessageW(list, LVM_SETITEMTEXTW, row, (LPARAM)&item);
        item.iSubItem = 2;
        item.pszText = points;
        SendMessageW(list, LVM_SETITEMTEXTW, row, (LPARAM)&item);
    }
}

static INT_PTR CALLBACK high_score_dialog_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam)
{
    HWND list = GetDlgItem(dialog, IDC_HIGH_SCORE_LIST);
    if (message == WM_INITDIALOG) {
        HighScoreDialog *state = (HighScoreDialog *)lparam;
        SetWindowLongPtrW(dialog, DWLP_USER, lparam);
        app_icon_set(dialog);
        RECT rect;
        GetClientRect(list, &rect);
        int width = rect.right;
        wchar_t *headers[] = {L"Rank", L"Name", L"Score"};
        int widths[] = {width * 16 / 100, width * 60 / 100, width * 24 / 100};
        for (int i = 0; i < 3; ++i) {
            LVCOLUMNW column = {.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT,
                .pszText = headers[i], .cx = widths[i], .fmt = i == 2 ? LVCFMT_RIGHT : LVCFMT_LEFT};
            SendMessageW(list, LVM_INSERTCOLUMNW, i, (LPARAM)&column);
        }
        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT);
        fill_scores(list, state->table);
        return TRUE;
    }
    if (message == WM_CLOSE || (message == WM_COMMAND &&
            (LOWORD(wparam) == IDOK || LOWORD(wparam) == IDCANCEL))) {
        EndDialog(dialog, IDOK);
        return TRUE;
    }
    if (message == WM_COMMAND && LOWORD(wparam) == IDC_RESET_HIGH_SCORES) {
        HighScoreDialog *state = (HighScoreDialog *)GetWindowLongPtrW(dialog, DWLP_USER);
        if (app_message_box(dialog, L"Restore the original all-time scores for this profile?",
                            L"Sorcery+", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
            if (state->reset()) fill_scores(list, state->table);
            else app_message_box(dialog, L"Could not reset the high-score table for this profile.",
                                 L"Sorcery+", MB_OK | MB_ICONWARNING);
        }
        return TRUE;
    }
    return FALSE;
}

bool high_score_dialog_show(HWND owner, const uint8_t *table, bool (*reset)(void))
{
    INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_LISTVIEW_CLASSES};
    if (!InitCommonControlsEx(&controls)) return false;
    HighScoreDialog state = {table, reset};
    return DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_HIGH_SCORES),
                           owner, high_score_dialog_proc, (LPARAM)&state) == IDOK;
}
