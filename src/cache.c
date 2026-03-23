#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// External verbose flag (defined in main.c)
extern int verbose;

// ---------------------------------------------------------------------------
// Decoded surface cache
//
// Caches decoded and rendered icon surfaces to avoid re-rendering on hover.
// Each icon has a cached decoded pixel buffer that can be reused.
// ---------------------------------------------------------------------------
typedef struct {
	char *icon_path;
	int size;
	uint32_t *pixels; // Decoded/rendered pixel data
} DecodedSurface;

// Global cache for decoded surfaces
static DecodedSurface *surface_cache = NULL;
static int surface_cache_count = 0;
static int surface_cache_capacity = 0;

// Initialize the surface cache
void
cache_init(void)
{
	surface_cache_capacity = 8;
	surface_cache = malloc(surface_cache_capacity * sizeof(DecodedSurface));
	surface_cache_count = 0;
	if (verbose >= 3)
		printf("[CACHE] Initialized surface cache (capacity: %d)\n",
			surface_cache_capacity);
}

// Free the surface cache
void
cache_free(void)
{
	for (int i = 0; i < surface_cache_count; i++) {
		free(surface_cache[i].icon_path);
		free(surface_cache[i].pixels);
	}
	free(surface_cache);
	surface_cache = NULL;
	surface_cache_count = 0;
	surface_cache_capacity = 0;
	if (verbose >= 3)
		printf("[CACHE] Freed surface cache\n");
}

// Look up a cached surface by icon path and size
// Returns pointer to pixel data, or NULL if not found
uint32_t *
cache_lookup(const char *icon_path, int size)
{
	if (!surface_cache || !icon_path)
		return NULL;

	for (int i = 0; i < surface_cache_count; i++) {
		if (surface_cache[i].size == size &&
			strcmp(surface_cache[i].icon_path, icon_path) == 0) {
			if (verbose >= 3)
				printf("[CACHE] HIT: %s (%dx%d)\n", icon_path, size, size);
			return surface_cache[i].pixels;
		}
	}

	if (verbose >= 3)
		printf("[CACHE] MISS: %s (%dx%d)\n", icon_path, size, size);
	return NULL;
}

// Store a decoded surface in the cache
// Copies the pixel data, so the caller can free their temporary buffer
void
cache_store(const char *icon_path, int size, uint32_t *pixels)
{
	if (!surface_cache || !icon_path || !pixels)
		return;

	// Grow cache if needed
	if (surface_cache_count >= surface_cache_capacity) {
		surface_cache_capacity *= 2;
		DecodedSurface *tmp = realloc(surface_cache,
			surface_cache_capacity * sizeof(DecodedSurface));
		if (!tmp) {
			if (verbose >= 3)
				printf("[CACHE] REALLOC FAILED - could not grow cache\n");
			return;
		}
		surface_cache = tmp;
	}

	// Allocate pixel data copy
	int pixel_size = size * size * 4;
	uint32_t *pixel_copy = malloc(pixel_size);
	if (!pixel_copy) {
		if (verbose >= 3)
			printf("[CACHE] MALLOC FAILED - could not cache surface\n");
		return;
	}

	memcpy(pixel_copy, pixels, pixel_size);

	surface_cache[surface_cache_count].icon_path = strdup(icon_path);
	surface_cache[surface_cache_count].pixels = pixel_copy;
	surface_cache[surface_cache_count].size = size;

	if (verbose >= 3)
		printf("[CACHE] STORE: %s (%dx%d) at index %d\n", icon_path, size, size,
			surface_cache_count);

	surface_cache_count++;
}

// Clear the cache (for when icons are reloaded or app exits)
void
cache_clear(void)
{
	for (int i = 0; i < surface_cache_count; i++) {
		free(surface_cache[i].icon_path);
		free(surface_cache[i].pixels);
	}
	surface_cache_count = 0;
	if (verbose >= 3)
		printf("[CACHE] Cleared all cached surfaces\n");
}
