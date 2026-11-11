#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <strsafe.h>
#include <wchar.h>

#include "save_dialog.h"
#include "resource.h"
#include "system_dialog_icon.h"

static HRESULT configure_dialog(IFileDialog *dialog, bool saving)
{
    static const COMDLG_FILTERSPEC types[] = {
        {L"Sorcery+ save files (*.sorceryplus; *.sav)", L"*.sorceryplus;*.sav"}
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
        result = IFileDialog_SetDefaultExtension(dialog, L"sorceryplus");
    }
    if (SUCCEEDED(result)) {
        result = IFileDialog_SetTitle(dialog, saving ? L"Save Sorcery+ progress"
                                                    : L"Load Sorcery+ progress");
    }
    return result;
}

static bool has_extension(const wchar_t *path, size_t length, const wchar_t *extension)
{
    size_t suffix = wcslen(extension);
    return length >= suffix &&
        CompareStringOrdinal(path + length - suffix, (int)suffix, extension, (int)suffix, TRUE) == CSTR_EQUAL;
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
        bool is_save_file = has_extension(chosen, length, L".sorceryplus") ||
                            has_extension(chosen, length, L".sav");
        if (is_save_file && length < capacity) {
            wmemcpy(path, chosen, length + 1);
        }
        CoTaskMemFree(chosen);
        if (is_save_file) {
            return length < capacity ? SAVE_DIALOG_SELECTED : SAVE_DIALOG_ERROR;
        }
        app_message_box(owner, L"Choose a .sorceryplus or .sav file.", L"Sorcery+",
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

static LSTATUS set_file_type_value(const wchar_t *key, const wchar_t *value)
{
    return RegSetKeyValueW(HKEY_CURRENT_USER, key, NULL, REG_SZ, value,
                           (DWORD)((wcslen(value) + 1) * sizeof(*value)));
}

bool save_dialog_register_icon(void)
{
    static const wchar_t extension[] = L"Software\\Classes\\.sorceryplus";
    static const wchar_t file_type[] = L"StuPot.SorceryPlus.Save";
    wchar_t existing[128] = {0};
    DWORD size = sizeof(existing);
    LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, extension, NULL, RRF_RT_REG_SZ, NULL, existing, &size);
    if ((status == ERROR_SUCCESS && *existing && _wcsicmp(existing, file_type)) ||
        (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)) return false;

    wchar_t executable[1024], icon[1056];
    DWORD length = GetModuleFileNameW(NULL, executable, ARRAYSIZE(executable));
    if (!length || length >= ARRAYSIZE(executable) ||
        FAILED(StringCchPrintfW(icon, ARRAYSIZE(icon), L"\"%ls\",-%d", executable, IDI_SORCERY_PLUS))) return false;
    if (set_file_type_value(L"Software\\Classes\\StuPot.SorceryPlus.Save", L"Sorcery+ saved game") != ERROR_SUCCESS ||
        set_file_type_value(L"Software\\Classes\\StuPot.SorceryPlus.Save\\DefaultIcon", icon) != ERROR_SUCCESS ||
        set_file_type_value(extension, file_type) != ERROR_SUCCESS) return false;
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
    return true;
}
