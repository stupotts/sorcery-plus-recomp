#ifndef SORCERY_HIGH_SCORES_H
#define SORCERY_HIGH_SCORES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    HIGH_SCORE_TABLE_SIZE = 168,
    HIGH_SCORES_SECTOR_SIZE = 512
};

/* Read a valid ledger into the score area, leaving the sector tail intact. */
bool high_scores_load(const wchar_t *path, uint8_t sector[HIGH_SCORES_SECTOR_SIZE]);

/* Merge a score area of at least HIGH_SCORE_TABLE_SIZE bytes. Return true if changed. */
bool high_scores_merge(uint8_t target[HIGH_SCORES_SECTOR_SIZE], const uint8_t *incoming);

/* Submit only additions beyond the caller's current table to the live ledger. */
bool high_scores_submit(const wchar_t *path, uint8_t sector[HIGH_SCORES_SECTOR_SIZE],
                        const uint8_t *incoming);

/* Import a legacy table only if the independent ledger has not been created. */
bool high_scores_import(const wchar_t *path, uint8_t sector[HIGH_SCORES_SECTOR_SIZE],
                        const uint8_t *legacy);

/* Replace the ledger with the supplied original score table. */
bool high_scores_reset(const wchar_t *path, uint8_t sector[HIGH_SCORES_SECTOR_SIZE]);

#endif
