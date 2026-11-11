#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>
#include <wchar.h>

#include "save_dialog.h"
#include "system_dialog_icon.h"

static HRESULT configure_dialog(IFileDialog *dialog, bool saving)
{
    static const COMDLG_FILTERSPEC types[] = {
        {L"Sorcery+ save files (*.sav)", L"*.sav"}
    };
    FILEOPENDIALOGOPTIONS options;
    HRESULT result = IFileDialog_GetOptions(dialog, &options);
    if (FAILED(result)) {
        return result;
    }
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR |
        FOS_DONTADDTORECENT;
    options |= saving ? FOS_OVERWRITEPROMPT | FOS_STRICTFILETYPES : FOS_FILEMUSTEXIST;
    result = IFileDialog_SetOptions(dialog, options);
    if (SUCCEEDED(result)) {
        result = IFileDialog_SetFileTypes(dialog, ARRAYSIZE(types), types);
    }
    if (SUCCEEDED(result)) {
        result = IFileDialog_SetDefaultExtension(dialog, L"sav");
    }
    if (SUCCEEDED(result)) {
        result = IFileDialog_SetTitle(dialog, saving ? L"Save Sorcery+ progress"
                                                    : L"Load Sorcery+ progress");
    }
    return result;
}

static SaveDialogResult choose_path(IFileDialog *dialog, HWND owner,
                                    wchar_t *path, size_t capacity)
{
    for (;;) {
        AppDialogIconScope scope;
        app_dialog_icon_begin(&scope, owner);
        HRESULT result = IFileDialog_Show(dialog, owner);
        app_dialog_icon_end(&scope);
        if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
            return SAVE_DIALOG_CANCELED;
        }
        if (FAILED(result)) {
            return SAVE_DIALOG_ERROR;
        }
        IShellItem *selection;
        if (FAILED(IFileDialog_GetResult(dialog, &selection))) {
            return SAVE_DIALOG_ERROR;
        }
        wchar_t *chosen;
        result = IShellItem_GetDisplayName(selection, SIGDN_FILESYSPATH, &chosen);
        IShellItem_Release(selection);
        if (FAILED(result)) {
            return SAVE_DIALOG_ERROR;
        }
        size_t length = wcslen(chosen);
        bool is_save_file = length >= 4 &&
            CompareStringOrdinal(chosen + length - 4, 4, L".sav", 4, TRUE) == CSTR_EQUAL;
        if (is_save_file && length < capacity) {
            wmemcpy(path, chosen, length + 1);
        }
        CoTaskMemFree(chosen);
        if (is_save_file) {
            return length < capacity ? SAVE_DIALOG_SELECTED : SAVE_DIALOG_ERROR;
        }
        app_message_box(owner, L"Choose a file with the .sav extension.", L"Sorcery+",
                        MB_OK | MB_ICONINFORMATION);
    }
}

SaveDialogResult save_dialog_choose(HWND owner, const wchar_t *initial_directory,
                                   bool saving, wchar_t *path, size_t capacity)
{
    if (!path || !capacity ||
        FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        return SAVE_DIALOG_ERROR;
    }
    SaveDialogResult choice = SAVE_DIALOG_ERROR;
    IFileDialog *dialog;
    const CLSID *class_id = saving ? &CLSID_FileSaveDialog : &CLSID_FileOpenDialog;
    HRESULT result = CoCreateInstance(class_id, NULL, CLSCTX_INPROC_SERVER,
                                      &IID_IFileDialog, (void **)&dialog);
    if (SUCCEEDED(result)) {
        if (SUCCEEDED(configure_dialog(dialog, saving))) {
            IShellItem *folder;
            if (initial_directory && *initial_directory &&
                SUCCEEDED(SHCreateItemFromParsingName(initial_directory, NULL, &IID_IShellItem,
                                                      (void **)&folder))) {
                IFileDialog_SetFolder(dialog, folder);
                IShellItem_Release(folder);
            }
            choice = choose_path(dialog, owner, path, capacity);
        }
        IFileDialog_Release(dialog);
    }
    CoUninitialize();
    return choice;
}
