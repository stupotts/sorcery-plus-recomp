#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <strsafe.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "room_atlas.h"
#include "embedded_assets.h"
#include "edition.h"
#include "storage.h"

enum { NOTE_LIMIT = 48002, FILE_LIMIT = 4000000, REQUEST_LIMIT = 8192, TOKEN_SIZE = 32 };
static char token[TOKEN_SIZE + 1];
static unsigned atlas_port;
static wchar_t notes_path[1120], lock_path[1120];

enum { LIVE_CAPACITY = 65536, ATLAS_ROOMS = 75, SANCTUARY = 0x1c };
static const char inactive_state[] = "{\"active\":false}\n";
static SRWLOCK live_lock = SRWLOCK_INIT;
static char live_state[LIVE_CAPACITY] = "{\"active\":false}\n";
static bool live_saved;

typedef struct { char data[LIVE_CAPACITY]; size_t size; bool failed; } LiveJson;

static void append_json(LiveJson *json, const char *format, ...)
{
    if (json->failed) return;
    size_t available = sizeof json->data - json->size;
    va_list args;
    va_start(args, format);
    int length = vsnprintf(json->data + json->size, available, format, args);
    va_end(args);
    if (length < 0 || (size_t)length >= available) json->failed = true;
    else json->size += (size_t)length;
}

static const uint8_t *atlas_room_record(const uint8_t *memory, unsigned room)
{
    // The game edits the current room at $6800 and commits it to the table on exit.
    return memory + (room == memory[SORCERY_ROOM_NUMBER] ? 0x6800 : 0x4000 + 64 * room);
}

void room_atlas_update(const uint8_t *memory, const wchar_t *path)
{
    if (memory[SORCERY_ROOM_NUMBER] >= ATLAS_ROOMS) return;
    LiveJson json = { .size = 0 };
    // $6840 is the player sprite record used by the original draw routines.
    append_json(&json, "{\"active\":true,\"room\":\"%02X\",\"held\":\"%02X\","
                "\"player\":{\"x\":%u,\"y\":%u,\"sprite\":\"%02X\"},\"rooms\":{",
                memory[SORCERY_ROOM_NUMBER], memory[0x6846], memory[0x6842], memory[0x6841], memory[0x6843]);
    for (unsigned room = 0; room < ATLAS_ROOMS; ++room) {
        const uint8_t *record = atlas_room_record(memory, room);
        append_json(&json, "%s\"%02X\":{\"items\":[", room ? "," : "", room);
        bool comma = false;
        for (unsigned slot = 0; slot < 4; ++slot) {
            const uint8_t *item = record + 0x34 + 3 * slot;
            if (item[0] == 0xff || item[2] == 0xff) continue;
            append_json(&json, "%s{\"slot\":%u,\"x\":%u,\"y\":%u,\"sprite\":\"%02X\"}",
                        comma ? "," : "", slot, item[1], item[0], item[2] & 0x7f);
            comma = true;
        }
        append_json(&json, "],\"monsters\":[");
        comma = false;
        for (unsigned slot = 0; slot < 2; ++slot) {
            const uint8_t *actor = record + 2 + 10 * slot;
            // States 8 and 9 are the defeated actor's exit animation.
            if ((actor[0] & 0x7f) >= 8 || actor[3] >= 96) continue;
            append_json(&json, "%s{\"slot\":%u,\"x\":%u,\"y\":%u,\"sprite\":\"%02X\"}",
                        comma ? "," : "", slot, actor[2], actor[1], actor[3]);
            comma = true;
        }
        append_json(&json, "],\"doorLocks\":[");
        for (unsigned slot = 0; slot < 4; ++slot) {
            const uint8_t *door = record + 0x16 + 4 * slot;
            if (slot) append_json(&json, ",");
            // The original door test at $7B7C masks out the destination slot.
            if (door[0] == 0xff) append_json(&json, "null");
            else append_json(&json, "%u", door[3] & 0x3f);
        }
        append_json(&json, "],\"prisoner\":%s}", record[0x32] == 0xff ? "false" : "true");
    }
    // These are the eight prisoner rooms in the original location table.
    static const uint8_t prisoner_rooms[] = {0x0e,0x14,0x19,0x1b,0x1e,0x25,0x27,0x2e};
    unsigned freed = 0;
    append_json(&json, "},\"freed\":[");
    for (unsigned i = 0; i < ARRAYSIZE(prisoner_rooms); ++i) {
        unsigned room = prisoner_rooms[i];
        if (atlas_room_record(memory, room)[0x32] == 0xff)
            append_json(&json, "%s\"%02X\"", freed++ ? "," : "", room);
    }
    append_json(&json, "],\"sanctuary\":[");
    for (unsigned i = 0; i < freed; ++i) {
        const uint8_t *actor = memory + (memory[SORCERY_ROOM_NUMBER] == SANCTUARY ? SORCERY_SANCTUARY_ACTORS : SORCERY_SANCTUARY_SLOTS) + 8 * i;
        if (actor[0] == 0xff) actor = memory + SORCERY_SANCTUARY_SLOTS + 8 * i;
        append_json(&json, "%s{\"x\":%u,\"y\":%u,\"sprite\":\"%02X\"}",
                    i ? "," : "", actor[2], actor[1], actor[3]);
    }
    append_json(&json, "]}\n");
    if (json.failed) return;
    if (strcmp(live_state, json.data)) {
        AcquireSRWLockExclusive(&live_lock);
        memcpy(live_state, json.data, json.size + 1);
        ReleaseSRWLockExclusive(&live_lock);
        live_saved = false;
    }
    if (!live_saved && path) live_saved = storage_publish(path, json.data, json.size);
}

void room_atlas_clear(void)
{
    AcquireSRWLockExclusive(&live_lock);
    memcpy(live_state, inactive_state, sizeof inactive_state);
    ReleaseSRWLockExclusive(&live_lock);
    live_saved = false;
}

static bool send_bytes(SOCKET client, const char *bytes, size_t size)
{
    while (size) {
        int count = send(client, bytes, (int)size, 0);
        if (count <= 0) return false;
        bytes += count;
        size -= count;
    }
    return true;
}

static void respond(SOCKET client, const char *status, const char *type, const void *data, size_t size)
{
    char header[512];
    int length = snprintf(header, sizeof header,
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n"
        "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n"
        "Content-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
        "img-src data:; connect-src 'self'; base-uri 'none'; frame-ancestors 'none'\r\n\r\n", status, type, size);
    if (length > 0 && (size_t)length < sizeof header && send_bytes(client, header, (size_t)length))
        send_bytes(client, data, size);
}

static char *read_notes(size_t *size)
{
    FILE *file = _wfopen(notes_path, L"rb");
    if (!file) {
        if (GetFileAttributesW(notes_path) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND) {
            *size = 3;
            return _strdup("{}\n");
        }
        return NULL;
    }
    char *contents = NULL;
    if (!fseek(file, 0, SEEK_END)) {
        long length = ftell(file);
        if (length > 0 && length <= FILE_LIMIT && !fseek(file, 0, SEEK_SET)) {
            contents = malloc((size_t)length + 1);
            if (contents && fread(contents, 1, (size_t)length, file) == (size_t)length) {
                contents[length] = '\0';
                *size = (size_t)length;
            } else {
                free(contents);
                contents = NULL;
            }
        }
    }
    fclose(file);
    return contents;
}

static int hex_digit(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1;
}

static int room_index(const char *id)
{
    int first = hex_digit(id[0]), second = hex_digit(id[1]);
    int index = first * 16 + second;
    return first >= 0 && second >= 0 && index < 75 ? index : -1;
}

static bool valid_json_string(const char *note, size_t size)
{
    if (size < 2 || size > NOTE_LIMIT || note[0] != '"' || note[size - 1] != '"' ||
        !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, note, (int)size, NULL, 0)) return false;
    for (size_t i = 1; i < size - 1; ++i) {
        if ((unsigned char)note[i] < 32 || note[i] == '"') return false;
        if (note[i] != '\\') continue;
        if (++i >= size - 1 || !strchr("\"\\/bfnrtu", note[i]) || !note[i]) return false;
        if (note[i] == 'u') {
            for (int n = 0; n < 4; ++n) if (++i >= size - 1 || hex_digit(note[i]) < 0) return false;
        }
    }
    return true;
}

typedef struct { const char *text; size_t size; } Note;

static bool parse_notes(const char *contents, size_t size, Note notes[75])
{
    if (size == 3 && !memcmp(contents, "{}\n", 3)) return true;
    if (size < 4 || memcmp(contents, "{\n", 2) || memcmp(contents + size - 2, "}\n", 2)) return false;
    const char *line = contents + 2, *end = contents + size - 2;
    while (line < end) {
        const char *next = memchr(line, '\n', (size_t)(end - line));
        if (!next || next - line < 7 || line[0] != '"' || line[3] != '"' || line[4] != ':') return false;
        int index = room_index(line + 1);
        size_t length = (size_t)(next - (line + 5));
        if (next + 1 < end) {
            if (next[-1] != ',') return false;
            --length;
        }
        if (index < 0 || notes[index].text || !valid_json_string(line + 5, length)) return false;
        notes[index] = (Note){line + 5, length};
        line = next + 1;
    }
    return true;
}

static bool write_note(int room, const char *note, size_t size)
{
    HANDLE lock = CreateFileW(lock_path, GENERIC_WRITE | DELETE, 0, NULL, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (lock == INVALID_HANDLE_VALUE) return false;
    size_t previous_size;
    char *previous = read_notes(&previous_size);
    Note notes[75] = {0};
    bool saved = false;
    if (previous && parse_notes(previous, previous_size, notes)) {
        notes[room] = size == 2 ? (Note){0} : (Note){note, size};
        char *output = malloc(previous_size + size + 16);
        if (output) {
            size_t length = 2;
            memcpy(output, "{\n", length);
            for (unsigned index = 0; index < ARRAYSIZE(notes); ++index) {
                if (!notes[index].text) continue;
                snprintf(output + length, 6, "\"%02X\":", index);
                length += 5;
                memcpy(output + length, notes[index].text, notes[index].size);
                length += notes[index].size;
                memcpy(output + length, ",\n", 2);
                length += 2;
            }
            if (length > 2) {
                output[length - 2] = '\n';
                --length;
            }
            memcpy(output + length, "}\n", 2);
            saved = storage_publish(notes_path, output, length + 2);
            free(output);
        }
    }
    free(previous);
    CloseHandle(lock);
    return saved;
}

static void serve(SOCKET client)
{
    char request[REQUEST_LIMIT + 1], method[8], uri[96], version[16], host[40];
    DWORD timeout = 2000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof timeout);
    char *body = NULL;
    size_t used = 0, content_length = 0;
    while (!body && used < REQUEST_LIMIT) {
        int count = recv(client, request + used, (int)(REQUEST_LIMIT - used), 0);
        if (count <= 0) return;
        used += count;
        request[used] = '\0';
        body = strstr(request, "\r\n\r\n");
    }
    if (!body || sscanf(request, "%7s %95s %15s", method, uri, version) != 3 ||
        (strcmp(version, "HTTP/1.1") && strcmp(version, "HTTP/1.0"))) return;
    snprintf(host, sizeof host, "Host: 127.0.0.1:%u", atlas_port);
    bool host_matches = false, has_length = false;
    for (char *line = strstr(request, "\r\n"); line && line < body;) {
        line += 2;
        char *next = strstr(line, "\r\n");
        if (!next) return;
        *next = '\0';
        if (!_stricmp(line, host)) host_matches = true;
        if (!_strnicmp(line, "Transfer-Encoding:", 18)) return;
        if (!_strnicmp(line, "Content-Length:", 15)) {
            char *end;
            if (has_length) return;
            content_length = strtoul(line + 15, &end, 10);
            if (end == line + 15 || *end || content_length > NOTE_LIMIT) return;
            has_length = true;
        }
        *next = '\r';
        line = next;
    }
    body += 4;
    if (!host_matches || uri[0] != '/' || strncmp(uri + 1, token, TOKEN_SIZE) || uri[TOKEN_SIZE + 1] != '/') {
        respond(client, "404 Not Found", "text/plain", "", 0);
        return;
    }
    const char *path = uri + TOKEN_SIZE + 2;
    if (!strcmp(method, "GET")) {
        bool guide = !strcmp(path, "Room-Guide.html");
        size_t size;
        if (guide || !strcmp(path, "Room-Atlas.html")) {
            const unsigned char *html = embedded_atlas_page(guide, &size);
            respond(client, "200 OK", "text/html; charset=utf-8", html, size);
            return;
        }
        if (!strcmp(path, "Room-Notes.json")) {
            char *notes = read_notes(&size);
            if (notes) respond(client, "200 OK", "application/json; charset=utf-8", notes, size);
            else respond(client, "500 Internal Server Error", "text/plain", "", 0);
            free(notes);
            return;
        }
        if (!strcmp(path, "Atlas-Live.json")) {
            AcquireSRWLockShared(&live_lock);
            char *snapshot = _strdup(live_state);
            ReleaseSRWLockShared(&live_lock);
            if (snapshot) respond(client, "200 OK", "application/json; charset=utf-8", snapshot, strlen(snapshot));
            else respond(client, "500 Internal Server Error", "text/plain", "", 0);
            free(snapshot);
            return;
        }
    }
    if (!strcmp(method, "PUT") && strlen(path) == 8 && !strncmp(path, "notes/", 6) && has_length) {
        int room = room_index(path + 6);
        char *note = malloc(content_length ? content_length : 1);
        if (!note) return;
        size_t copied = used - (size_t)(body - request);
        if (copied > content_length) copied = content_length;
        memcpy(note, body, copied);
        while (copied < content_length) {
            int count = recv(client, note + copied, (int)(content_length - copied), 0);
            if (count <= 0) break;
            copied += count;
        }
        const char *status = "400 Bad Request";
        if (room >= 0 && copied == content_length && valid_json_string(note, copied))
            status = write_note(room, note, copied) ? "204 No Content" : "500 Internal Server Error";
        respond(client, status, "text/plain", "", 0);
        free(note);
        return;
    }
    respond(client, "404 Not Found", "text/plain", "", 0);
}

static DWORD WINAPI atlas_thread(void *argument)
{
    SOCKET listener = (SOCKET)(UINT_PTR)argument;
    for (;;) {
        SOCKET client = accept(listener, NULL, NULL);
        if (client == INVALID_SOCKET) break;
        serve(client);
        shutdown(client, SD_SEND);
        closesocket(client);
    }
    closesocket(listener);
    WSACleanup();
    return 0;
}

static bool start_atlas(void)
{
    wchar_t directory[1120];
    DWORD length = GetModuleFileNameW(NULL, directory, ARRAYSIZE(directory));
    if (!length || length >= ARRAYSIZE(directory)) return false;
    wchar_t *filename = wcsrchr(directory, L'\\');
    if (!filename) return false;
    filename[1] = L'\0';
    if (FAILED(StringCchCatW(directory, ARRAYSIZE(directory), L"profiles"))) return false;
    if (!CreateDirectoryW(directory, NULL)) {
        DWORD attributes = GetFileAttributesW(directory);
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    }
    if (FAILED(StringCchPrintfW(notes_path, ARRAYSIZE(notes_path), L"%ls\\Room-Notes.json", directory)) ||
        FAILED(StringCchPrintfW(lock_path, ARRAYSIZE(lock_path), L"%ls\\.room-notes.lock", directory))) return false;

    unsigned char random_bytes[TOKEN_SIZE / 2];
    if (BCryptGenRandom(NULL, random_bytes, sizeof random_bytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return false;
    for (unsigned i = 0; i < sizeof random_bytes; ++i) snprintf(token + i * 2, 3, "%02x", random_bytes[i]);
    WSADATA winsock;
    if (WSAStartup(MAKEWORD(2, 2), &winsock)) return false;
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    int address_size = sizeof address;
    if (listener != INVALID_SOCKET && !bind(listener, (struct sockaddr *)&address, sizeof address) &&
        !listen(listener, 8) && !getsockname(listener, (struct sockaddr *)&address, &address_size)) {
        atlas_port = ntohs(address.sin_port);
        HANDLE thread = CreateThread(NULL, 0, atlas_thread, (void *)(UINT_PTR)listener, 0, NULL);
        if (thread) {
            CloseHandle(thread);
            return true;
        }
        atlas_port = 0;
    }
    if (listener != INVALID_SOCKET) closesocket(listener);
    WSACleanup();
    return false;
}

bool room_atlas_open(HWND owner)
{
    if (!atlas_port && !start_atlas()) return false;
    wchar_t url[128];
    if (FAILED(StringCchPrintfW(url, ARRAYSIZE(url), L"http://127.0.0.1:%u/%hs/Room-Atlas.html", atlas_port, token))) return false;
    return (INT_PTR)ShellExecuteW(owner, L"open", url, NULL, NULL, SW_SHOWNORMAL) > 32;
}
