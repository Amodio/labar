#ifndef CACHE_H
#define CACHE_H

#include <stdint.h>

// Initialize the surface cache
void cache_init(void);

// Free the surface cache
void cache_free(void);

// Look up a cached surface by icon path and size
// Returns pointer to pixel data, or NULL if not found
uint32_t *cache_lookup(const char *icon_path, int size);

// Store a decoded surface in the cache
// Copies the pixel data, so the caller can free their temporary buffer
void cache_store(const char *icon_path, int size, uint32_t *pixels);

// Clear the cache (for when icons are reloaded or app exits)
void cache_clear(void);

#endif // CACHE_H
