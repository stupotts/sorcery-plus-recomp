#ifndef SORCERY_EMBEDDED_ASSETS_H
#define SORCERY_EMBEDDED_ASSETS_H

#include <stdbool.h>
#include <stddef.h>

bool embedded_asset_read(const char *name, void *bytes, size_t capacity, size_t *size);
// UTF-8 with Windows line endings. Size, when requested, excludes the NUL.
const char *embedded_launch_guide(size_t *size);
const unsigned char *embedded_atlas_page(bool guide, size_t *size);

#endif
