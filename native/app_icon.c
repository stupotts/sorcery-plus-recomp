#define WIN32_LEAN_AND_MEAN
#include "app_icon.h"
#include "resource.h"

HICON app_icon(bool small)
{
    // Window classes and dialogs share these handles for the life of the process.
    static HICON icons[2];
    if (icons[small]) return icons[small];
    int width = GetSystemMetrics(small ? SM_CXSMICON : SM_CXICON);
    int height = GetSystemMetrics(small ? SM_CYSMICON : SM_CYICON);
    icons[small] = LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_SORCERY_PLUS),
                              IMAGE_ICON, width, height, 0);
    if (!icons[small])
        icons[small] = LoadImageW(NULL, MAKEINTRESOURCEW(32512), IMAGE_ICON, width, height, LR_SHARED);
    return icons[small];
}

void app_icon_set(HWND window)
{
    SendMessageW(window, WM_SETICON, ICON_BIG, (LPARAM)app_icon(false));
    SendMessageW(window, WM_SETICON, ICON_SMALL, (LPARAM)app_icon(true));
}
