#ifndef SORCERY_STORAGE_H
#define SORCERY_STORAGE_H

#include <stdbool.h>
#include <stddef.h>

/* Replace a save only after the new payload has been written and flushed. */
bool storage_save(const wchar_t *path, const void *payload, size_t size);
bool storage_publish(const wchar_t *path, const void *payload, size_t size);

/* Load an exact-size payload. On failure, leave the caller's buffer alone. */
bool storage_load(const wchar_t *path, void *payload, size_t size);

#endif
