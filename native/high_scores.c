#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>

#include "high_scores.h"
#include "storage.h"

enum {
    RECORD_SIZE = 24,
    SCORE_OFFSET = HIGH_SCORE_NAME_LENGTH,
    SCORE_SIZE = 3
};

_Static_assert(HIGH_SCORE_COUNT * RECORD_SIZE == HIGH_SCORE_TABLE_SIZE, "Score table size mismatch");

HighScoreEntry high_scores_entry(const uint8_t *table, unsigned index)
{
    const uint8_t *record = table + index * RECORD_SIZE;
    HighScoreEntry entry = {0};
    size_t length = HIGH_SCORE_NAME_LENGTH;
    while (length && (record[length - 1] == '.' || record[length - 1] == ' ')) --length;
    for (size_t i = 0; i < length; ++i) {
        unsigned character = record[i] & 0x7f;
        entry.name[i] = character >= 32 && character <= 126 ? (wchar_t)character : L'?';
    }
    for (size_t i = SCORE_OFFSET; i < RECORD_SIZE; ++i)
        entry.points = entry.points * 100 + (record[i] >> 4) * 10 + (record[i] & 15);
    return entry;
}

static int compare_score(const uint8_t *first, const uint8_t *second)
{
    return memcmp(first + SCORE_OFFSET, second + SCORE_OFFSET, SCORE_SIZE);
}

static bool valid_table(const uint8_t *table)
{
    if (!table) {
        return false;
    }
    for (size_t i = 0; i < HIGH_SCORE_COUNT; ++i) {
        const uint8_t *record = table + i * RECORD_SIZE;
        for (size_t j = SCORE_OFFSET; j < RECORD_SIZE; ++j) {
            if ((record[j] & 15u) > 9 || (record[j] >> 4) > 9) {
                return false;
            }
        }
        if (i && compare_score(record - RECORD_SIZE, record) < 0) {
            return false;
        }
    }
    return true;
}

static unsigned record_count(const uint8_t *table, size_t count, const uint8_t *record)
{
    unsigned matches = 0;
    for (size_t i = 0; i < count; ++i) {
        matches += memcmp(table + i * RECORD_SIZE, record, RECORD_SIZE) == 0;
    }
    return matches;
}

static void merge_additions(uint8_t *target, const uint8_t *baseline, const uint8_t *incoming)
{
    for (size_t i = 0; i < HIGH_SCORE_COUNT; ++i) {
        const uint8_t *record = incoming + i * RECORD_SIZE;
        if (record_count(incoming, i + 1, record) <= record_count(baseline, HIGH_SCORE_COUNT, record)) {
            continue;
        }
        size_t rank = 0;
        while (rank < HIGH_SCORE_COUNT && compare_score(target + rank * RECORD_SIZE, record) >= 0) {
            ++rank;
        }
        if (rank < HIGH_SCORE_COUNT) {
            uint8_t *position = target + rank * RECORD_SIZE;
            memmove(position + RECORD_SIZE, position, (HIGH_SCORE_COUNT - rank - 1) * RECORD_SIZE);
            memcpy(position, record, RECORD_SIZE);
        }
    }
}

bool high_scores_merge(uint8_t target[HIGH_SCORES_SECTOR_SIZE], const uint8_t *incoming)
{
    if (!valid_table(target) || !valid_table(incoming)) {
        return false;
    }
    uint8_t merged[HIGH_SCORE_TABLE_SIZE];
    memcpy(merged, target, sizeof(merged));
    // A record present in both tables counts only once per occurrence.
    merge_additions(merged, target, incoming);
    if (memcmp(target, merged, sizeof(merged)) == 0) {
        return false;
    }
    memcpy(target, merged, sizeof(merged));
    return true;
}

bool high_scores_load(const wchar_t *path, uint8_t sector[HIGH_SCORES_SECTOR_SIZE])
{
    uint8_t table[HIGH_SCORE_TABLE_SIZE];
    if (!sector || !storage_load(path, table, sizeof(table)) || !valid_table(table)) {
        return false;
    }
    memcpy(sector, table, sizeof(table));
    return true;
}

static HANDLE lock_ledger(void)
{
    // Serialize read/merge/write across game instances in this Windows session.
    // A fixed name also covers two aliases of the same profile path.
    HANDLE lock = CreateMutexW(NULL, FALSE, L"Local\\SorceryPlusHighScores-v1");
    if (!lock) {
        return NULL;
    }
    DWORD wait = WaitForSingleObject(lock, 5000);
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
        return lock;
    }
    CloseHandle(lock);
    return NULL;
}

static void unlock_ledger(HANDLE lock)
{
    ReleaseMutex(lock);
    CloseHandle(lock);
}

bool high_scores_submit(const wchar_t *path, uint8_t sector[HIGH_SCORES_SECTOR_SIZE],
                        const uint8_t *incoming)
{
    if (!path || !*path || !valid_table(sector) || !valid_table(incoming)) {
        return false;
    }
    HANDLE lock = lock_ledger();
    if (!lock) {
        return false;
    }
    uint8_t existing[HIGH_SCORE_TABLE_SIZE];
    uint8_t latest[HIGH_SCORE_TABLE_SIZE];
    bool have_ledger = storage_load(path, existing, sizeof(existing)) && valid_table(existing);
    memcpy(latest, have_ledger ? existing : sector, sizeof(latest));
    // Submit only the new records. A window opened before a reset may still
    // hold pre-reset scores in both its baseline and the game's write buffer.
    merge_additions(latest, sector, incoming);
    bool saved = (have_ledger && memcmp(latest, existing, sizeof(latest)) == 0) ||
        storage_save(path, latest, sizeof(latest));
    if (saved) {
        memcpy(sector, latest, sizeof(latest));
    }
    unlock_ledger(lock);
    return saved;
}

bool high_scores_import(const wchar_t *path, uint8_t sector[HIGH_SCORES_SECTOR_SIZE],
                        const uint8_t *legacy)
{
    if (!path || !*path || !valid_table(sector)) {
        return false;
    }
    HANDLE lock = lock_ledger();
    if (!lock) {
        return false;
    }
    bool imported = high_scores_load(path, sector);
    if (!imported && GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            uint8_t latest[HIGH_SCORE_TABLE_SIZE];
            memcpy(latest, sector, sizeof(latest));
            if (valid_table(legacy)) {
                merge_additions(latest, sector, legacy);
            }
            imported = memcmp(latest, sector, sizeof(latest)) == 0 ||
                storage_save(path, latest, sizeof(latest));
            if (imported) {
                memcpy(sector, latest, sizeof(latest));
            }
        }
    }
    unlock_ledger(lock);
    return imported;
}

bool high_scores_reset(const wchar_t *path, uint8_t sector[HIGH_SCORES_SECTOR_SIZE])
{
    if (!path || !*path || !valid_table(sector)) {
        return false;
    }
    HANDLE lock = lock_ledger();
    if (!lock) {
        return false;
    }
    bool saved = storage_save(path, sector, HIGH_SCORE_TABLE_SIZE);
    unlock_ledger(lock);
    return saved;
}
