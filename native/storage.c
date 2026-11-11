#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "storage.h"

enum {
    MAGIC_SIZE = 8,
    VERSION_OFFSET = 8,
    LENGTH_OFFSET = 12,
    CHECKSUM_OFFSET = 20,
    HEADER_SIZE = 24,
    IO_CHUNK_SIZE = 1024 * 1024,
    TEMP_ATTEMPTS = 32
};

static const uint8_t magic[MAGIC_SIZE] = {'S', 'O', 'R', 'C', 'P', 'L', 'U', 'S'};
static const uint32_t version = 1;
static volatile LONG temp_sequence;

static void put_le(uint8_t *destination, uint64_t value, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; ++i) {
        destination[i] = (uint8_t)value;
        value >>= 8;
    }
}

static uint64_t get_le(const uint8_t *source, unsigned bytes)
{
    uint64_t value = 0;
    while (bytes != 0) {
        value = (value << 8) | source[--bytes];
    }
    return value;
}

static uint32_t checksum(const uint8_t *data, size_t size)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & mask);
        }
    }
    return ~crc;
}

static bool valid_arguments(const wchar_t *path, const void *payload, size_t size)
{
    return path && *path && (payload || size == 0) &&
        (uint64_t)size <= (uint64_t)INT64_MAX - HEADER_SIZE;
}

static bool write_exact(HANDLE file, const void *buffer, size_t size)
{
    const uint8_t *data = buffer;
    while (size != 0) {
        DWORD chunk = size > IO_CHUNK_SIZE ? IO_CHUNK_SIZE : (DWORD)size;
        DWORD written;
        if (!WriteFile(file, data, chunk, &written, NULL) || written == 0) {
            return false;
        }
        data += written;
        size -= written;
    }
    return true;
}

static bool read_exact(HANDLE file, void *buffer, size_t size)
{
    uint8_t *data = buffer;
    while (size != 0) {
        DWORD chunk = size > IO_CHUNK_SIZE ? IO_CHUNK_SIZE : (DWORD)size;
        DWORD received;
        if (!ReadFile(file, data, chunk, &received, NULL) || received == 0) {
            return false;
        }
        data += received;
        size -= received;
    }
    return true;
}

static void put_hex(wchar_t *destination, uint32_t value)
{
    static const wchar_t digits[] = L"0123456789abcdef";
    for (unsigned i = 0; i < 8; ++i) {
        destination[7 - i] = digits[value & 15u];
        value >>= 4;
    }
}

static HANDLE open_temp(const wchar_t *path, wchar_t **temp_path)
{
    static const wchar_t template[] = L".sorcery-00000000-00000000.tmp";
    size_t directory = path[1] == L':' ? 2 : 0;
    for (size_t i = directory; path[i]; ++i) {
        if (path[i] == L'\\' || path[i] == L'/') {
            directory = i + 1;
        }
    }
    if (directory > (SIZE_MAX - sizeof(template)) / sizeof(wchar_t)) {
        return INVALID_HANDLE_VALUE;
    }
    wchar_t *name = HeapAlloc(GetProcessHeap(), 0, directory * sizeof(wchar_t) + sizeof(template));
    if (!name) {
        return INVALID_HANDLE_VALUE;
    }
    memcpy(name, path, directory * sizeof(wchar_t));
    memcpy(name + directory, template, sizeof(template));
    put_hex(name + directory + 9, GetCurrentProcessId());
    for (unsigned attempt = 0; attempt < TEMP_ATTEMPTS; ++attempt) {
        put_hex(name + directory + 18, (uint32_t)InterlockedIncrement(&temp_sequence));
        HANDLE file = CreateFileW(name, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE) {
            *temp_path = name;
            return file;
        }
        if (GetLastError() != ERROR_FILE_EXISTS) {
            break;
        }
    }
    HeapFree(GetProcessHeap(), 0, name);
    return INVALID_HANDLE_VALUE;
}

static bool write_atomic(const wchar_t *path, const void *header, size_t header_size,
                         const void *payload, size_t size)
{
    wchar_t *temp_path;
    HANDLE file = open_temp(path, &temp_path);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    bool written = write_exact(file, header, header_size) &&
        write_exact(file, payload, size) && FlushFileBuffers(file);
    bool closed = CloseHandle(file);
    bool saved = written && closed &&
        MoveFileExW(temp_path, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!saved) {
        DeleteFileW(temp_path);
    }
    HeapFree(GetProcessHeap(), 0, temp_path);
    return saved;
}

bool storage_save(const wchar_t *path, const void *payload, size_t size)
{
    if (!valid_arguments(path, payload, size)) return false;
    uint8_t header[HEADER_SIZE];
    memcpy(header, magic, MAGIC_SIZE);
    put_le(header + VERSION_OFFSET, version, 4);
    put_le(header + LENGTH_OFFSET, size, 8);
    put_le(header + CHECKSUM_OFFSET, checksum(payload, size), 4);
    return write_atomic(path, header, sizeof(header), payload, size);
}

bool storage_publish(const wchar_t *path, const void *payload, size_t size)
{
    return valid_arguments(path, payload, size) && write_atomic(path, NULL, 0, payload, size);
}

bool storage_load(const wchar_t *path, void *payload, size_t size)
{
    if (!valid_arguments(path, payload, size)) {
        return false;
    }
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    uint8_t header[HEADER_SIZE];
    LARGE_INTEGER file_size;
    bool valid = GetFileSizeEx(file, &file_size) &&
        file_size.QuadPart == (LONGLONG)(HEADER_SIZE + (uint64_t)size) &&
        read_exact(file, header, sizeof(header)) &&
        memcmp(header, magic, MAGIC_SIZE) == 0 &&
        get_le(header + VERSION_OFFSET, 4) == version &&
        get_le(header + LENGTH_OFFSET, 8) == (uint64_t)size;
    uint8_t *staged = valid && size != 0 ? HeapAlloc(GetProcessHeap(), 0, size) : NULL;
    valid = valid && (staged || size == 0) && read_exact(file, staged, size) &&
        checksum(staged, size) == get_le(header + CHECKSUM_OFFSET, 4);
    if (valid && size != 0) {
        memcpy(payload, staged, size);
    }
    if (staged) {
        HeapFree(GetProcessHeap(), 0, staged);
    }
    CloseHandle(file);
    return valid;
}
