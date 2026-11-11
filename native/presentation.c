#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#include <strsafe.h>
#include <string.h>
#include <wchar.h>
#include "presentation.h"
#include "app_icon.h"
#include "audio_dialog.h"
#include "help_dialog.h"
#include "keyboard_dialog.h"
#include "room_atlas.h"
#include "system_dialog_icon.h"

enum {
    SOURCE_WIDTH = 640,
    SOURCE_HEIGHT = 200,
    ROW_BYTES = 80,
    PREFERENCES_PATH_CAPACITY = 1120
};

enum {
    MENU_FULLSCREEN = 100,
    MENU_SHARP,
    MENU_SMOOTH,
    MENU_ASPECT_CRT,
    MENU_ASPECT_RAW,
    MENU_SAVE,
    MENU_LOAD,
    MENU_CLOSE,
    MENU_EXIT_TO_TITLE,
    MENU_KEYBOARD,
    MENU_AUDIO,
    MENU_RESET_HIGH_SCORES,
    MENU_LAUNCH_GUIDE,
    MENU_ROOM_ATLAS,
    MENU_ABOUT
};

enum { ACTION_UP, ACTION_DOWN, ACTION_LEFT, ACTION_RIGHT, ACTION_FIRE, ACTION_PAUSE, ACTION_COUNT };

typedef DWORD (WINAPI *XInputStateFunction)(DWORD, XINPUT_STATE *);

static const WCHAR windowClassName[] = L"SorceryPlusNative";
static const WCHAR windowTitle[] = L"Sorcery+";
static const unsigned defaultBindings[ACTION_COUNT] = {
    VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, 'Z', VK_SPACE
};
static const WCHAR *const bindingNames[ACTION_COUNT] = {
    L"Up", L"Down", L"Left", L"Right", L"Fire", L"Pause"
};

static HWND window;
static HMENU windowMenu;
static HMENU displayMenu;
static WINDOWPLACEMENT windowedPlacement = { .length = sizeof(WINDOWPLACEMENT) };
static HMODULE controllerLibrary;
static XInputStateFunction getControllerState;
static ULONGLONG retryControllerAt;
static uint8_t keyRows[10];
static unsigned bindings[ACTION_COUNT];
static unsigned suppressedKey;
static unsigned pendingActions;
static AudioSettings audioSettings = {
    .sfx_enabled = true, .music_enabled = true, .sfx_volume = 100, .music_volume = 50
};
static bool menuActive;
static bool fullscreen;
static bool smooth;
static bool rawAspect;
static bool hasFrame;
static int savedWidth;
static int savedHeight;
static WCHAR preferencesPath[PREFERENCES_PATH_CAPACITY];
static uint32_t framePixels[SOURCE_WIDTH * SOURCE_HEIGHT];

static const BITMAPINFO frameInfo = {
    .bmiHeader = {
        .biSize = sizeof(BITMAPINFOHEADER),
        .biWidth = SOURCE_WIDTH,
        .biHeight = -SOURCE_HEIGHT,
        .biPlanes = 1,
        .biBitCount = 32,
        .biCompression = BI_RGB
    }
};

/* Amstrad CPC keyboard rows, indexed by active-low bit number. */
static const unsigned matrixKeys[10][8] = {
    {VK_UP, VK_RIGHT, VK_DOWN, VK_NUMPAD9, VK_NUMPAD6, VK_NUMPAD3, 0, VK_DECIMAL},
    {VK_LEFT, VK_INSERT, VK_NUMPAD7, VK_NUMPAD8, VK_NUMPAD5, VK_NUMPAD1, VK_NUMPAD2, VK_NUMPAD0},
    {VK_DELETE, VK_OEM_4, VK_RETURN, VK_OEM_6, VK_NUMPAD4, VK_SHIFT, VK_OEM_5, VK_CONTROL},
    {VK_OEM_3, VK_OEM_MINUS, 0, 'P', VK_OEM_1, VK_OEM_7, VK_OEM_2, VK_OEM_PERIOD},
    {'0', '9', 'O', 'I', 'L', 'K', 'M', VK_OEM_COMMA},
    {'8', '7', 'U', 'Y', 'H', 'J', 'N', 0},
    {'6', '5', 'R', 'T', 'G', 'F', 'B', 'V'},
    {'4', '3', 'E', 'W', 'S', 'D', 'C', 'X'},
    {'1', '2', VK_ESCAPE, 'Q', VK_TAB, 'A', VK_CAPITAL, 'Z'},
    {0, 0, 0, 0, 0, 0, 0, VK_BACK}
};

static bool validBinding(unsigned key)
{
    return key >= 8 && key <= 254 && key != VK_ESCAPE &&
           key != VK_F5 && key != VK_F9 && key != VK_F11;
}

static bool preferencesFileExists(const WCHAR *path)
{
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static void importLegacyPreferences(const WCHAR *legacy, const WCHAR *directory)
{
    WCHAR staged[MAX_PATH];
    if (!GetTempFileNameW(directory, L"Sor", 0, staged)) return;
    if (CopyFileW(legacy, staged, FALSE) &&
        SetFileAttributesW(staged, FILE_ATTRIBUTE_NORMAL) &&
        MoveFileExW(staged, preferencesPath, MOVEFILE_WRITE_THROUGH)) return;
    SetFileAttributesW(staged, FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(staged);
}

static const WCHAR *preparePreferencesPath(WCHAR legacy[PREFERENCES_PATH_CAPACITY])
{
    WCHAR directory[PREFERENCES_PATH_CAPACITY];
    preferencesPath[0] = L'\0';
    DWORD length = GetModuleFileNameW(NULL, directory, ARRAYSIZE(directory));
    if (!length || length >= ARRAYSIZE(directory)) return NULL;
    WCHAR *slash = wcsrchr(directory, L'\\');
    if (!slash) return NULL;
    slash[1] = L'\0';
    if (FAILED(StringCchPrintfW(legacy, PREFERENCES_PATH_CAPACITY, L"%lsSorceryPlus.ini", directory))) return NULL;
    if (FAILED(StringCchCatW(directory, ARRAYSIZE(directory), L"profiles")) ||
        FAILED(StringCchPrintfW(preferencesPath, ARRAYSIZE(preferencesPath), L"%ls\\SorceryPlus.ini", directory))) {
        preferencesPath[0] = L'\0';
        return preferencesFileExists(legacy) ? legacy : NULL;
    }
    if (preferencesFileExists(preferencesPath)) return preferencesPath;

    const bool hasLegacy = preferencesFileExists(legacy);
    if (!CreateDirectoryW(directory, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return hasLegacy ? legacy : preferencesPath;
    }
    if (hasLegacy) importLegacyPreferences(legacy, directory);
    if (preferencesFileExists(preferencesPath)) return preferencesPath;
    return hasLegacy ? legacy : preferencesPath;
}

static int savedDimension(const WCHAR *path, const WCHAR *name, int fallback)
{
    unsigned size = GetPrivateProfileIntW(L"Display", name, fallback, path);
    return size >= 200 && size <= 8192 ? (int)size : fallback;
}

static unsigned savedVolume(const WCHAR *path, const WCHAR *name, unsigned fallback)
{
    unsigned volume = GetPrivateProfileIntW(L"Audio", name, fallback, path);
    return volume <= 100 ? volume : fallback;
}

static void readPreferences(void)
{
    WCHAR legacy[PREFERENCES_PATH_CAPACITY];
    const WCHAR *path = preparePreferencesPath(legacy);
    savedWidth = 800;
    savedHeight = 600;
    memcpy(bindings, defaultBindings, sizeof(bindings));
    if (!path) return;
    savedWidth = savedDimension(path, L"Width", savedWidth);
    savedHeight = savedDimension(path, L"Height", savedHeight);
    fullscreen = GetPrivateProfileIntW(L"Display", L"Fullscreen", 0, path) == 1;
    smooth = GetPrivateProfileIntW(L"Display", L"Smooth", 0, path) == 1;
    rawAspect = GetPrivateProfileIntW(L"Display", L"RawAspect", 0, path) == 1;
    audioSettings.sfx_enabled = GetPrivateProfileIntW(L"Audio", L"SoundEffectsEnabled", 1, path) != 0;
    audioSettings.music_enabled = GetPrivateProfileIntW(L"Audio", L"BackgroundMusicEnabled", 1, path) != 0;
    audioSettings.sfx_volume = savedVolume(path, L"SoundEffectsVolume", 100);
    audioSettings.music_volume = savedVolume(path, L"BackgroundMusicVolume", 50);
    for (unsigned i = 0; i < ACTION_COUNT; ++i) {
        unsigned key = GetPrivateProfileIntW(L"Keyboard", bindingNames[i], bindings[i], path);
        if (validBinding(key)) bindings[i] = key;
    }
}

static void saveSetting(const WCHAR *section, const WCHAR *name, unsigned value)
{
    WCHAR text[16];
    if (SUCCEEDED(StringCchPrintfW(text, ARRAYSIZE(text), L"%u", value))) {
        WritePrivateProfileStringW(section, name, text, preferencesPath);
    }
}

static void savePreferences(void)
{
    if (!preferencesPath[0]) return;
    saveSetting(L"Display", L"Width", savedWidth);
    saveSetting(L"Display", L"Height", savedHeight);
    saveSetting(L"Display", L"Fullscreen", fullscreen);
    saveSetting(L"Display", L"Smooth", smooth);
    saveSetting(L"Display", L"RawAspect", rawAspect);
    saveSetting(L"Audio", L"SoundEffectsEnabled", audioSettings.sfx_enabled);
    saveSetting(L"Audio", L"BackgroundMusicEnabled", audioSettings.music_enabled);
    saveSetting(L"Audio", L"SoundEffectsVolume", audioSettings.sfx_volume);
    saveSetting(L"Audio", L"BackgroundMusicVolume", audioSettings.music_volume);
    for (unsigned i = 0; i < ACTION_COUNT; ++i) {
        saveSetting(L"Keyboard", bindingNames[i], bindings[i]);
    }
}

static void toggleFullscreen(void)
{
    if (fullscreen) {
        fullscreen = false;
        SetWindowLongPtrW(window, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetMenu(window, windowMenu);
        SetWindowPlacement(window, &windowedPlacement);
        SetWindowPos(window, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    } else {
        MONITORINFO info = { .cbSize = sizeof(MONITORINFO) };
        HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
        if (!GetMonitorInfoW(monitor, &info) || !GetWindowPlacement(window, &windowedPlacement)) {
            return;
        }
        fullscreen = true;
        SetMenu(window, NULL);
        SetWindowLongPtrW(window, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(window, HWND_TOP, info.rcMonitor.left, info.rcMonitor.top,
                     info.rcMonitor.right - info.rcMonitor.left,
                     info.rcMonitor.bottom - info.rcMonitor.top, SWP_FRAMECHANGED);
    }
    savePreferences();
}

static void updateMenu(void)
{
    CheckMenuItem(displayMenu, MENU_FULLSCREEN, MF_BYCOMMAND | (fullscreen ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuRadioItem(displayMenu, MENU_SHARP, MENU_SMOOTH,
                       smooth ? MENU_SMOOTH : MENU_SHARP, MF_BYCOMMAND);
    CheckMenuRadioItem(displayMenu, MENU_ASPECT_CRT, MENU_ASPECT_RAW,
                       rawAspect ? MENU_ASPECT_RAW : MENU_ASPECT_CRT, MF_BYCOMMAND);
}

static void paintFrame(HWND hwnd, HDC context)
{
    RECT client;
    if (!GetClientRect(hwnd, &client)) return;
    if (client.right == 0 || client.bottom == 0) return;
    HBRUSH black = GetStockObject(BLACK_BRUSH);
    if (!hasFrame) {
        FillRect(context, &client, black);
        return;
    }

    int aspectWidth = rawAspect ? 8 : 4;
    int aspectHeight = rawAspect ? 5 : 3;
    int width = client.right;
    int height = client.bottom;
    if (width * aspectHeight > height * aspectWidth) {
        width = height * aspectWidth / aspectHeight;
    } else {
        height = width * aspectHeight / aspectWidth;
    }
    int x = (client.right - width) / 2;
    int y = (client.bottom - height) / 2;
    if (width < client.right) {
        RECT left = {0, 0, x, client.bottom};
        RECT right = {x + width, 0, client.right, client.bottom};
        FillRect(context, &left, black);
        FillRect(context, &right, black);
    } else if (height < client.bottom) {
        RECT top = {0, 0, client.right, y};
        RECT bottom = {0, y + height, client.right, client.bottom};
        FillRect(context, &top, black);
        FillRect(context, &bottom, black);
    }

    SetStretchBltMode(context, smooth ? HALFTONE : COLORONCOLOR);
    if (smooth) SetBrushOrgEx(context, 0, 0, NULL);
    StretchDIBits(context, x, y, width, height, 0, 0, SOURCE_WIDTH, SOURCE_HEIGHT,
                  framePixels, &frameInfo, DIB_RGB_COLORS, SRCCOPY);
}

static void suppressDialogKey(void)
{
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) suppressedKey = VK_ESCAPE;
    else if (GetAsyncKeyState(VK_RETURN) & 0x8000) suppressedKey = VK_RETURN;
}

static void runCommand(unsigned command)
{
    switch (command) {
        case MENU_SAVE:
            pendingActions |= DISPLAY_ACTION_SAVE;
            return;
        case MENU_LOAD:
            pendingActions |= DISPLAY_ACTION_LOAD;
            return;
        case MENU_CLOSE:
            pendingActions |= DISPLAY_ACTION_CLOSE;
            return;
        case MENU_EXIT_TO_TITLE:
            pendingActions |= DISPLAY_ACTION_EXIT_TO_TITLE;
            return;
        case MENU_RESET_HIGH_SCORES:
            pendingActions |= DISPLAY_ACTION_RESET_HIGH_SCORES;
            return;
        case MENU_LAUNCH_GUIDE:
            if (!help_dialog_show(window)) display_notice(L"Could not open the launch guide.", true);
            suppressDialogKey();
            return;
        case MENU_ROOM_ATLAS:
            if (!room_atlas_open(window)) display_notice(L"Could not open the room atlas. Check that the game folder is writable and a default browser is set.", true);
            suppressDialogKey();
            return;
        case MENU_ABOUT:
            if (!about_dialog_show(window)) display_notice(L"Could not open About Sorcery+.", true);
            suppressDialogKey();
            return;
        case MENU_KEYBOARD:
        case MENU_AUDIO: {
            bool accepted = command == MENU_KEYBOARD
                ? keyboard_dialog_show(window, bindings, defaultBindings)
                : audio_dialog_show(window, &audioSettings);
            if (accepted) {
                savePreferences();
                if (command == MENU_AUDIO) pendingActions |= DISPLAY_ACTION_AUDIO_SETTINGS;
            }
            suppressDialogKey();
            return;
        }
        case MENU_FULLSCREEN:
            toggleFullscreen();
            return;
        case MENU_SHARP:
        case MENU_SMOOTH:
            smooth = command == MENU_SMOOTH;
            break;
        case MENU_ASPECT_CRT:
        case MENU_ASPECT_RAW:
            rawAspect = command == MENU_ASPECT_RAW;
            break;
        default:
            return;
    }
    savePreferences();
    InvalidateRect(window, NULL, FALSE);
}

static LRESULT CALLBACK windowProcedure(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (message == WM_SYSKEYDOWN && wParam == VK_F4 && (lParam & (1L << 29))) {
                if (!(lParam & (1L << 30))) runCommand(MENU_CLOSE);
                return 0;
            }
            if (message == WM_KEYDOWN && wParam == 'T' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                if (!(lParam & (1L << 30))) {
                    suppressedKey = 'T';
                    runCommand(MENU_EXIT_TO_TITLE);
                }
                return 0;
            }
            if (message == WM_KEYDOWN && wParam == VK_ESCAPE && fullscreen) {
                if (!(lParam & (1L << 30))) {
                    suppressedKey = VK_ESCAPE;
                    toggleFullscreen();
                }
                return 0;
            }
            if (wParam == VK_F11 || (wParam == VK_RETURN && message == WM_SYSKEYDOWN)) {
                if (!(lParam & (1L << 30))) toggleFullscreen();
                return 0;
            }
            if (wParam == VK_F5 || wParam == VK_F9) {
                if (!(lParam & (1L << 30))) runCommand(wParam == VK_F5 ? MENU_SAVE : MENU_LOAD);
                return 0;
            }
            break;
        case WM_INITMENU:
            updateMenu();
            return 0;
        case WM_ENTERMENULOOP:
            menuActive = true;
            return 0;
        case WM_EXITMENULOOP:
            menuActive = false;
            return 0;
        case WM_COMMAND:
            runCommand(LOWORD(wParam));
            return 0;
        case WM_CLOSE:
            runCommand(MENU_CLOSE);
            return 0;
        case WM_SIZE:
            if (!fullscreen && wParam == SIZE_RESTORED) {
                savedWidth = LOWORD(lParam);
                savedHeight = HIWORD(lParam);
            }
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        case WM_GETMINMAXINFO:
            ((MINMAXINFO *)lParam)->ptMinTrackSize.x = 320;
            ((MINMAXINFO *)lParam)->ptMinTrackSize.y = 240;
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint;
            HDC context = BeginPaint(hwnd, &paint);
            paintFrame(hwnd, context);
            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_DESTROY:
            savePreferences();
            window = NULL;
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

static HMENU createMenus(void)
{
    HMENU root = CreateMenu();
    HMENU file = CreatePopupMenu();
    HMENU settings = CreatePopupMenu();
    HMENU view = CreatePopupMenu();
    HMENU help = CreatePopupMenu();
    if (!root || !file || !settings || !view || !help) {
        if (root) DestroyMenu(root);
        if (file) DestroyMenu(file);
        if (settings) DestroyMenu(settings);
        if (view) DestroyMenu(view);
        if (help) DestroyMenu(help);
        return NULL;
    }
    AppendMenuW(file, MF_STRING, MENU_LOAD, L"Load Game\tF9");
    AppendMenuW(file, MF_STRING, MENU_SAVE, L"Save Game\tF5");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, MENU_RESET_HIGH_SCORES, L"Reset High Scores...");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, MENU_CLOSE, L"Close\tAlt+F4");
    AppendMenuW(file, MF_STRING, MENU_EXIT_TO_TITLE, L"Exit to Title\tCtrl+T");
    AppendMenuW(view, MF_STRING, MENU_FULLSCREEN, L"Borderless fullscreen\tF11");
    AppendMenuW(view, MF_SEPARATOR, 0, NULL);
    AppendMenuW(view, MF_STRING, MENU_SHARP, L"Sharp pixels");
    AppendMenuW(view, MF_STRING, MENU_SMOOTH, L"Smooth scaling");
    AppendMenuW(view, MF_SEPARATOR, 0, NULL);
    AppendMenuW(view, MF_STRING, MENU_ASPECT_CRT, L"Classic 4:3 aspect");
    AppendMenuW(view, MF_STRING, MENU_ASPECT_RAW, L"Wide 8:5 aspect");
    AppendMenuW(settings, MF_POPUP, (UINT_PTR)view, L"&Display");
    AppendMenuW(settings, MF_STRING, MENU_KEYBOARD, L"&Keyboard...");
    AppendMenuW(settings, MF_STRING, MENU_AUDIO, L"&Audio...");
    AppendMenuW(root, MF_POPUP, (UINT_PTR)file, L"&File");
    AppendMenuW(root, MF_POPUP, (UINT_PTR)settings, L"&Settings");
    AppendMenuW(help, MF_STRING, MENU_LAUNCH_GUIDE, L"&Launch Guide");
    AppendMenuW(help, MF_STRING, MENU_ROOM_ATLAS, L"Room &Atlas");
    AppendMenuW(help, MF_STRING, MENU_ABOUT, L"&About");
    AppendMenuW(root, MF_POPUP, (UINT_PTR)help, L"&Help");
    displayMenu = view;
    return root;
}

static bool isPressed(unsigned key)
{
    return key && key != suppressedKey && (GetAsyncKeyState((int)key) & 0x8000) != 0;
}

static void addController(bool actions[ACTION_COUNT])
{
    if (!getControllerState || GetTickCount64() < retryControllerAt) return;

    XINPUT_STATE state;
    if (getControllerState(0, &state) != ERROR_SUCCESS) {
        retryControllerAt = GetTickCount64() + 1000;
        return;
    }
    const XINPUT_GAMEPAD *pad = &state.Gamepad;
    const WORD buttons = pad->wButtons;
    const SHORT deadzone = XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE;
    actions[ACTION_UP] |= (buttons & XINPUT_GAMEPAD_DPAD_UP) != 0 || pad->sThumbLY > deadzone;
    actions[ACTION_DOWN] |= (buttons & XINPUT_GAMEPAD_DPAD_DOWN) != 0 || pad->sThumbLY < -deadzone;
    actions[ACTION_LEFT] |= (buttons & XINPUT_GAMEPAD_DPAD_LEFT) != 0 || pad->sThumbLX < -deadzone;
    actions[ACTION_RIGHT] |= (buttons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0 || pad->sThumbLX > deadzone;
    actions[ACTION_FIRE] |= (buttons & XINPUT_GAMEPAD_A) != 0;
    actions[ACTION_PAUSE] |= (buttons & XINPUT_GAMEPAD_START) != 0;
}

static void pollInputs(void)
{
    memset(keyRows, 0xff, sizeof(keyRows));
    if (GetForegroundWindow() != window || menuActive) return;
    if (suppressedKey && !(GetAsyncKeyState((int)suppressedKey) & 0x8000)) suppressedKey = 0;
    for (unsigned row = 0; row < ARRAYSIZE(matrixKeys); ++row) {
        for (unsigned bit = 0; bit < ARRAYSIZE(matrixKeys[row]); ++bit) {
            if (isPressed(matrixKeys[row][bit])) keyRows[row] &= (uint8_t)~(1u << bit);
        }
    }

    bool actions[ACTION_COUNT];
    for (unsigned i = 0; i < ACTION_COUNT; ++i) actions[i] = isPressed(bindings[i]);
    addController(actions);
    for (unsigned i = ACTION_UP; i <= ACTION_RIGHT; ++i) {
        if (actions[i]) keyRows[9] &= (uint8_t)~(1u << i);
    }
    if (actions[ACTION_FIRE]) keyRows[9] &= (uint8_t)~0x30u;
    if (actions[ACTION_PAUSE]) keyRows[5] &= (uint8_t)~0x80u;
}

bool display_open(void)
{
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSEXW windowClass = {
        .cbSize = sizeof(windowClass),
        .lpfnWndProc = windowProcedure,
        .hInstance = instance,
        .hIcon = app_icon(false),
        .hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512)),
        .lpszClassName = windowClassName,
        .hIconSm = app_icon(true)
    };
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    readPreferences();
    const bool startFullscreen = fullscreen;
    fullscreen = false;
    hasFrame = false;
    suppressedKey = 0;
    pendingActions = 0;
    memset(keyRows, 0xff, sizeof(keyRows));
    windowMenu = createMenus();
    if (!windowMenu) return false;
    RECT rect = {0, 0, savedWidth, savedHeight};
    AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, TRUE, 0);
    window = CreateWindowExW(0, windowClassName, windowTitle, WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left,
                             rect.bottom - rect.top, NULL, windowMenu, instance, NULL);
    if (!window) {
        DestroyMenu(windowMenu);
        windowMenu = NULL;
        return false;
    }
    controllerLibrary = LoadLibraryExW(L"xinput1_4.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (controllerLibrary) {
        getControllerState = (XInputStateFunction)(void *)GetProcAddress(controllerLibrary, "XInputGetState");
    }
    retryControllerAt = 0;
    ShowWindow(window, SW_SHOW);
    if (startFullscreen) toggleFullscreen();
    return true;
}

bool display_pump(void)
{
    MSG message;
    while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) return false;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (!window) return false;
    pollInputs();
    return true;
}

HWND display_owner(void)
{
    return window;
}

static uint32_t inkColour(unsigned ink)
{
    static const uint32_t levels[3] = {0, 128, 255};
    if (ink > 26) ink = 0;
    return (levels[(ink / 3) % 3] << 16) | (levels[ink / 9] << 8) | levels[ink % 3];
}

static unsigned pixelPen(unsigned bits, unsigned mode)
{
    if (mode == 2) return (bits >> 7) & 1;
    unsigned pen = ((bits >> 7) & 1) | ((bits >> 2) & 2);
    if (mode == 0) pen |= ((bits >> 3) & 4) | ((bits << 2) & 8);
    return pen;
}

static void buildByteTable(uint32_t table[256][8], const uint8_t *inks, unsigned mode)
{
    uint32_t palette[16];
    for (unsigned pen = 0; pen < ARRAYSIZE(palette); ++pen) {
        palette[pen] = inkColour(inks[pen]);
    }
    if (mode > 2) mode = 0;
    unsigned scale = 4u >> mode;
    for (unsigned byte = 0; byte < 256; ++byte) {
        for (unsigned x = 0; x < 8; ++x) {
            table[byte][x] = palette[pixelPen(byte << (x / scale), mode)];
        }
    }
}

void display_frame_split(const uint8_t *vram, const uint8_t *topInks,
                         const uint8_t *bottomInks, unsigned startAddress,
                         int splitLine, int topMode, int bottomMode)
{
    uint32_t topTable[256][8], bottomTable[256][8];
    buildByteTable(topTable, topInks, topMode);
    if (splitLine < SOURCE_HEIGHT) buildByteTable(bottomTable, bottomInks, bottomMode);
    const unsigned base = startAddress & 0xc000;
    for (int y = 0; y < SOURCE_HEIGHT; ++y) {
        unsigned raster = base | ((unsigned)(y & 7) << 11);
        unsigned lineOffset = (startAddress & 0x7ff) + (unsigned)(y >> 3) * ROW_BYTES;
        uint32_t (*table)[8] = y < splitLine ? topTable : bottomTable;
        uint32_t *pixels = framePixels + y * SOURCE_WIDTH;
        for (unsigned byte = 0; byte < ROW_BYTES; ++byte) {
            unsigned value = vram[raster | ((lineOffset + byte) & 0x7ff)];
            memcpy(pixels + byte * 8, table[value], sizeof(table[value]));
        }
    }
    hasFrame = true;
    if (window) {
        InvalidateRect(window, NULL, FALSE);
        UpdateWindow(window);
    }
}

void display_frame(const uint8_t *vram, const uint8_t *inks,
                   unsigned startAddress, int splitLine, int topMode, int bottomMode)
{
    display_frame_split(vram, inks, inks, startAddress, splitLine, topMode, bottomMode);
}

static bool writeSnapshotPart(HANDLE file, const void *data, DWORD length)
{
    DWORD written = 0;
    return WriteFile(file, data, length, &written, NULL) && written == length;
}

bool display_snapshot(const wchar_t *path)
{
    if (!hasFrame) return false;
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    BITMAPINFOHEADER image = frameInfo.bmiHeader;
    image.biSizeImage = sizeof(framePixels);
    BITMAPFILEHEADER header = {
        .bfType = 0x4d42,
        .bfSize = sizeof(header) + sizeof(image) + sizeof(framePixels),
        .bfOffBits = sizeof(header) + sizeof(image)
    };
    bool ok = writeSnapshotPart(file, &header, sizeof(header)) &&
              writeSnapshotPart(file, &image, sizeof(image)) &&
              writeSnapshotPart(file, framePixels, sizeof(framePixels));
    if (!CloseHandle(file)) ok = false;
    if (!ok) DeleteFileW(path);
    return ok;
}

uint8_t display_key_row(unsigned row)
{
    return window && row < ARRAYSIZE(keyRows) ? keyRows[row] : 0xff;
}

unsigned display_take_action(void)
{
    unsigned actions = pendingActions;
    pendingActions = 0;
    return actions;
}

void display_get_audio_settings(AudioSettings *settings)
{
    *settings = audioSettings;
}

bool display_confirm(const wchar_t *message)
{
    return window && app_message_box(window, message, windowTitle,
                                     MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2) == IDOK;
}

void display_notice(const wchar_t *message, bool error)
{
    if (window) app_message_box(window, message, windowTitle, MB_OK | (error ? MB_ICONERROR : MB_ICONINFORMATION));
}

void display_close(void)
{
    /* Windows owns the attached menu. An unattached fullscreen menu is ours. */
    if (window) DestroyWindow(window);
    if (fullscreen && windowMenu) DestroyMenu(windowMenu);
    windowMenu = NULL;
    if (controllerLibrary) FreeLibrary(controllerLibrary);
    controllerLibrary = NULL;
    getControllerState = NULL;
}
