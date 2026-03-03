#define _GNU_SOURCE // Required for memfd_create()

#include <cairo.h>
#include <fcntl.h>
#include <getopt.h>
#include <librsvg/rsvg.h>
#include <linux/input-event-codes.h>
#include <locale.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "cache.h"
#include "config.h"
#include "exec.h"
#include "seat.h"
#include "widget-date.h"
#include "widget-volume.h"
#include "wlr-layer-shell-unstable-v1-protocol.h"

// Global verbose flag for debug output (0=none, 1=normal, 2=extra)
int verbose = 0;

// Global config — loaded once in main() and used by layer_configure()
Config app_config = {0};

// ---------------------------------------------------------------------------
// Wayland global objects — populated during registry enumeration
// ---------------------------------------------------------------------------
struct wl_display *display;
struct wl_registry *registry;
struct wl_compositor *compositor;
struct wl_shm *shm;
struct zwlr_layer_shell_v1 *layer_shell;
struct wl_seat *seat;
struct wl_pointer *pointer;

// ---------------------------------------------------------------------------
// Layer-surface state
// ---------------------------------------------------------------------------
struct wl_surface *surface;
struct zwlr_layer_surface_v1 *layer_surface;
struct wl_buffer *buffer;
uint32_t *pixels; // Pointer into the SHM mapping

// Surface dimensions — updated on the first configure event from the compositor
int surf_width = 64;
int surf_height = 64;

// Current pointer position (updated by pointer_motion)
double current_pointer_x = 0.0;
double current_pointer_y = 0.0;

// Track the previously hovered icon to avoid spam
int last_hovered_icon = -1;

// ---------------------------------------------------------------------------
// get_total_widget_count
//
// Returns the total number of icon slots: apps + optional volume + optional
// date widgets.
// ---------------------------------------------------------------------------
static int
get_total_widget_count(void)
{
	return app_config.count + (app_config.show_volume ? 1 : 0) +
		(app_config.show_date ? 1 : 0);
}

// ---------------------------------------------------------------------------
// get_date_slot_index
//
// Returns the slot index of the date widget, or -1 if disabled.
// The date widget is always the last slot.
// ---------------------------------------------------------------------------
int
get_date_slot_index(void)
{
	if (!app_config.show_date)
		return -1;
	return app_config.count + (app_config.show_volume ? 1 : 0);
}

// ---------------------------------------------------------------------------
// get_icon_at_position
//
// Calculate which icon (if any) is at the given coordinate.
// For horizontal layouts (TOP/BOTTOM), uses x coordinate.
// For vertical layouts (LEFT/RIGHT), uses y coordinate.
//
// Parameters:
//   coord – horizontal (x) or vertical (y) coordinate in pixels
//
// Returns the icon index (0 to total_count-1) or -1 if no icon is at that
// position.
// ---------------------------------------------------------------------------
int
get_icon_at_position(double coord)
{
	if (coord < 0)
		return -1;

	int icon_size = app_config.icon_size;
	int spacing = app_config.icon_spacing;
	int date_slot = app_config.show_date ?
		(app_config.count + (app_config.show_volume ? 1 : 0)) :
		-1;
	int total_count = get_total_widget_count();

	int pos = 0;
	for (int i = 0; i < total_count; i++) {
		int slot_size = (i == date_slot && app_config.date_tile_width > 0) ?
			app_config.date_tile_width :
			icon_size;
		int slot_end = pos + slot_size;
		if (coord >= pos && coord < slot_end)
			return i;
		pos = slot_end + spacing;
	}

	return -1;
}

// Function to calculate offset for a given icon index, accounting for spacing
// For horizontal layouts, returns x offset. For vertical layouts, returns y
// offset.
int
get_offset_for_icon(int icon_index)
{
	int total_count = get_total_widget_count();
	if (icon_index < 0 || icon_index >= total_count)
		return 0;

	int icon_size = app_config.icon_size;
	int spacing = app_config.icon_spacing;
	int date_slot = app_config.show_date ?
		(app_config.count + (app_config.show_volume ? 1 : 0)) :
		-1;

	int pos = 0;
	for (int i = 0; i < icon_index; i++) {
		int slot_size = (i == date_slot && app_config.date_tile_width > 0) ?
			app_config.date_tile_width :
			icon_size;
		pos += slot_size + spacing;
	}
	return pos;
}

// ---------------------------------------------------------------------------
// create_shm_buffer
//
// Allocates a shared-memory buffer via a memfd and wraps it in a wl_buffer.
// The caller receives a pointer to the pixel data through `out_data`.
//
// Parameters:
//   width    – buffer width in pixels
//   height   – buffer height in pixels
//   out_data – receives a pointer to the mapped ARGB pixel data
//
// Returns a wl_buffer bound to the SHM pool.
// ---------------------------------------------------------------------------
static struct wl_buffer *
create_shm_buffer(int width, int height, uint32_t **out_data)
{
	int stride = width * 4; // 4 bytes per pixel (ARGB8888)
	int size = stride * height;

	// Create an anonymous in-memory file to back the shared buffer
	int fd = memfd_create("icon-buffer", 0);
	if (fd < 0) {
		perror("memfd_create");
		exit(1);
	}
	if (ftruncate(fd, size) < 0) {
		perror("ftruncate");
		close(fd);
		exit(1);
	}

	// Map the file into our address space so we can write pixel data
	void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		perror("mmap");
		close(fd);
		exit(1);
	}
	*out_data = (uint32_t *)map;

	// Create a Wayland SHM pool from the fd, then carve out a single buffer
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, width, height,
		stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool); // The buffer keeps a reference; pool can go
	close(fd);				   // The mapping keeps the data alive

	return buf;
}

// ---------------------------------------------------------------------------
// draw_text
//
// Renders text onto the given pixel buffer using Cairo.
// The text is centered horizontally and its baseline sits at y_offset,
// measured from the top of the buffer (positive y points down).
//
// Parameters:
//   data     – pointer to the ARGB8888 pixel buffer
//   width    – buffer width in pixels
//   height   – buffer height in pixels
//   text     – the text string to render
//   y_offset – distance from the top of the buffer to the text baseline
//   color    – text color as 0xAARRGGBB
// ---------------------------------------------------------------------------
void
draw_text(uint32_t *data, int width, int height, const char *text, int y_offset,
	unsigned int color)
{
	if (!text || text[0] == '\0')
		return;

	// Wrap the pixel buffer in a Cairo image surface
	cairo_surface_t *cs =
		cairo_image_surface_create_for_data((unsigned char *)data,
			CAIRO_FORMAT_ARGB32, width, height, width * 4);
	cairo_t *cr = cairo_create(cs);

	// Set font and text properties
	cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
		CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, app_config.label_size);

	// Get text extents to center it
	cairo_text_extents_t extents;
	cairo_text_extents(cr, text, &extents);

	// Decode 0xAARRGGBB color
	double a = ((color >> 24) & 0xFF) / 255.0;
	double r = ((color >> 16) & 0xFF) / 255.0;
	double g = ((color >> 8) & 0xFF) / 255.0;
	double b = ((color) & 0xFF) / 255.0;
	cairo_set_source_rgba(cr, r, g, b, a);

	// Center the text horizontally.
	// y_offset is the distance from the top to the baseline; subtracting
	// y_bearing (which is negative for normal glyphs) moves the reference
	// point so the visible top of the text lands just above y_offset.
	double x = (width - extents.width) / 2.0 - extents.x_bearing;
	double y = (double)y_offset;
	cairo_move_to(cr, x, y);
	cairo_show_text(cr, text);

	cairo_destroy(cr);
	cairo_surface_destroy(cs);
}

// ---------------------------------------------------------------------------
// draw_svg
//
// Renders an SVG file into the given pixel buffer using Cairo + librsvg.
// The SVG is scaled uniformly to fill (width × height).
//
// Parameters:
//   path   – filesystem path to the SVG file
//   data   – pointer to the ARGB8888 pixel buffer
//   width  – target width in pixels
//   height – target height in pixels
// ---------------------------------------------------------------------------
void
draw_svg(const char *path, uint32_t *data, int width, int height)
{
	if (verbose >= 3)
		printf("[I/O] FOPEN (read SVG): %s\n", path);

	GError *error = NULL;
	RsvgHandle *handle = rsvg_handle_new_from_file(path, &error);
	if (!handle) {
		if (verbose >= 3)
			printf("[I/O] FOPEN FAILED: %s\n", path);
		fprintf(stderr, "Failed to load SVG '%s': %s\n", path,
			error ? error->message : "unknown error");
		if (error)
			g_error_free(error);
		return;
	}

	// Wrap the pixel buffer in a Cairo image surface (ARGB32 == ARGB8888)
	cairo_surface_t *cs =
		cairo_image_surface_create_for_data((unsigned char *)data,
			CAIRO_FORMAT_ARGB32, width, height, width * 4);
	cairo_t *cr = cairo_create(cs);

	// Clear to fully transparent before rendering
	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	cairo_paint(cr);

	// Determine the SVG's intrinsic size so we can scale correctly
	double svg_w = 0, svg_h = 0;
	if (!rsvg_handle_get_intrinsic_size_in_pixels(handle, &svg_w, &svg_h) ||
		svg_w == 0 || svg_h == 0) {
		// Intrinsic size unavailable — fall back to a safe default
		svg_w = 128;
		svg_h = 128;
	}

	// Scale the Cairo context so the SVG fills the target dimensions
	cairo_scale(cr, (double)width / svg_w, (double)height / svg_h);

	// Render the full document into a viewport matching its intrinsic size
	RsvgRectangle viewport = {0, 0, svg_w, svg_h};
	rsvg_handle_render_document(handle, cr, &viewport, NULL);

	cairo_destroy(cr);
	cairo_surface_destroy(cs);
	g_object_unref(handle);
}

void
draw_png(const char *path, uint32_t *data, int width, int height)
{
	if (verbose >= 3)
		printf("[I/O] FOPEN (read PNG): %s\n", path);

	cairo_surface_t *img = cairo_image_surface_create_from_png(path);
	if (cairo_surface_status(img) != CAIRO_STATUS_SUCCESS) {
		if (verbose >= 3)
			printf("[I/O] FOPEN FAILED: %s\n", path);
		fprintf(stderr, "Failed to load PNG '%s'\n", path);
		cairo_surface_destroy(img);
		return;
	}

	int img_w = cairo_image_surface_get_width(img);
	int img_h = cairo_image_surface_get_height(img);

	cairo_surface_t *cs =
		cairo_image_surface_create_for_data((unsigned char *)data,
			CAIRO_FORMAT_ARGB32, width, height, width * 4);
	cairo_t *cr = cairo_create(cs);

	// Clear target
	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	cairo_paint(cr);

	// Scale to fit
	cairo_scale(cr, (double)width / img_w, (double)height / img_h);

	cairo_set_source_surface(cr, img, 0, 0);
	cairo_paint(cr);

	cairo_destroy(cr);
	cairo_surface_destroy(cs);
	cairo_surface_destroy(img);
}

void
draw_icon(const char *path, uint32_t *data, int width, int height)
{
	// Check cache first
	uint32_t *cached = cache_lookup(path, width);
	if (cached) {
		if (verbose >= 2)
			printf("[DBG²] Using cached surface for %s (%dx%d)\n", path, width,
				height);
		memcpy(data, cached, width * height * 4);
		return;
	}

	const char *ext = strrchr(path, '.');
	if (!ext) {
		fprintf(stderr, "Icon has no extension: %s\n", path);
		return;
	}

	if (strcasecmp(ext, ".svg") == 0) {
		draw_svg(path, data, width, height);
	} else if (strcasecmp(ext, ".png") == 0) {
		draw_png(path, data, width, height);
	} else {
		fprintf(stderr, "Unsupported icon format: %s\n", path);
	}

	// Cache the decoded surface for future use
	cache_store(path, width, data);
}

// ---------------------------------------------------------------------------
// draw_tile
//
// Helper: allocate a zeroed icon_size² tile, call draw_icon() + optionally
// draw_text(), then blit it into the main pixel buffer at the correct offset.
// Frees the tile when done.
//
// Parameters:
//   icon_path  – path passed to draw_icon()
//   label      – text to overlay, or NULL / "" to skip
//   slot_index – index used to calculate the blit offset
//   is_vertical – non-zero for LEFT/RIGHT bar orientations
// ---------------------------------------------------------------------------
static void
draw_tile(const char *icon_path, const char *label, int slot_index,
	int is_vertical)
{
	int icon_size = app_config.icon_size;
	int offset = get_offset_for_icon(slot_index);

	uint32_t *tile = malloc(icon_size * icon_size * 4);
	if (!tile)
		return;
	memset(tile, 0, icon_size * icon_size * 4);

	draw_icon(icon_path, tile, icon_size, icon_size);

	if (label && label[0] != '\0') {
		int baseline = icon_size - app_config.label_offset;
		draw_text(tile, icon_size, icon_size, label, baseline,
			app_config.label_color);
	}

	if (is_vertical) {
		for (int ty = 0; ty < icon_size; ty++) {
			uint32_t *src = tile + ty * icon_size;
			uint32_t *dst = pixels + (offset + ty) * surf_width;
			memcpy(dst, src, icon_size * 4);
		}
	} else {
		for (int ty = 0; ty < icon_size; ty++) {
			uint32_t *src = tile + ty * icon_size;
			uint32_t *dst = pixels + ty * surf_width + offset;
			memcpy(dst, src, icon_size * 4);
		}
	}

	free(tile);
}

// ---------------------------------------------------------------------------
// date_repaint_tile
//
// Re-renders the date/time icon slot after a minute boundary is crossed.
// Called from the main dispatch loop via date_widget_needs_repaint().
// ---------------------------------------------------------------------------
void
date_repaint_tile(struct wl_surface *wl_surf)
{
	if (!buffer || !app_config.show_date)
		return;

	int is_vertical = (app_config.position == POSITION_LEFT ||
		app_config.position == POSITION_RIGHT);
	int slot = get_date_slot_index();
	int offset = get_offset_for_icon(slot);
	int icon_size = app_config.icon_size;
	// Along-bar dimension may be wider than icon_size to fit the text;
	// the cross-bar dimension is always icon_size.
	int tile_w =
		app_config.date_tile_width > 0 ? app_config.date_tile_width : icon_size;
	int tile_h = icon_size;

	uint32_t *tile = malloc(tile_w * tile_h * 4);
	if (!tile)
		return;

	date_draw_tile(tile, tile_w, tile_h, &app_config);

	if (is_vertical) {
		// Vertical bar: offset is along Y; tile_w is the height, tile_h is the
		// width
		for (int ty = 0; ty < tile_w; ty++) {
			uint32_t *src = tile + ty * tile_h;
			uint32_t *dst = pixels + (offset + ty) * surf_width;
			memcpy(dst, src, tile_h * 4);
		}
	} else {
		// Horizontal bar: offset is along X; tile_w is the width, tile_h is the
		// height
		for (int ty = 0; ty < tile_h; ty++) {
			uint32_t *src = tile + ty * tile_w;
			uint32_t *dst = pixels + ty * surf_width + offset;
			memcpy(dst, src, tile_w * 4);
		}
	}
	free(tile);

	wl_surface_attach(wl_surf, buffer, 0, 0);
	wl_surface_damage(wl_surf, 0, 0, surf_width, surf_height);
	wl_surface_commit(wl_surf);
}

// ---------------------------------------------------------------------------
// Layer-surface event listeners
// ---------------------------------------------------------------------------

// Called by the compositor when it assigns dimensions to our layer surface.
// This is where we create the SHM buffer and perform the first render.
static void
layer_configure(void *data, struct zwlr_layer_surface_v1 *surf, uint32_t serial,
	uint32_t width, uint32_t height)
{
	// Use compositor-provided dimensions, falling back to our defaults
	surf_width = width > 0 ? (int)width : 64;
	surf_height = height > 0 ? (int)height : 64;

	// Allocate the SHM buffer and render icons on the first configure only.
	// Subsequent configures (e.g. output hotplug) don't need a new buffer
	// unless the size changes — extend this block if resize support is
	// needed.
	if (!buffer) {
		buffer = create_shm_buffer(surf_width, surf_height, &pixels);

		// Clear the entire buffer to transparent
		memset(pixels, 0, surf_width * surf_height * 4);

		int icon_size = app_config.icon_size;		// Use configured icon size
		int icon_spacing = app_config.icon_spacing; // Use configured spacing

		// Determine if bar is horizontal or vertical
		int is_vertical = (app_config.position == POSITION_LEFT ||
			app_config.position == POSITION_RIGHT);

		if (is_vertical) {
			// Draw each application icon vertically (top to bottom)
			int y_offset = 0;
			for (int i = 0; i < app_config.count; i++) {
				if (y_offset + icon_size > surf_height)
					break; // Don't draw beyond buffer height

				if (app_config.apps[i]->icon) {
					if (verbose >= 2) {
						printf(
							"[DBG²] Drawing icon %d at y=%d: %s (size=%dx%d)\n",
							i, y_offset, app_config.apps[i]->icon, icon_size,
							icon_size);
					}

					// Create a temporary buffer for this icon tile
					uint32_t *tile_data = malloc(icon_size * icon_size * 4);
					memset(tile_data, 0, icon_size * icon_size * 4);

					// Draw the SVG onto the tile buffer
					draw_icon(app_config.apps[i]->icon, tile_data, icon_size,
						icon_size);

					// Draw the app name as a text overlay on the icon
					uint32_t *text_overlay = malloc(icon_size * icon_size * 4);
					memcpy(text_overlay, tile_data, icon_size * icon_size * 4);

					free(tile_data);

					// Draw the label only when label_mode is ALWAYS
					if (app_config.label_mode == LABEL_MODE_ALWAYS) {
						int baseline = icon_size - app_config.label_offset;
						draw_text(text_overlay, icon_size, icon_size,
							app_config.apps[i]->name, baseline,
							app_config.label_color);
					}

					// Blit the text overlay onto the main buffer
					for (int ty = 0; ty < icon_size; ty++) {
						uint32_t *src = text_overlay + (ty * icon_size);
						uint32_t *dst = pixels + ((y_offset + ty) * surf_width);
						memcpy(dst, src, icon_size * 4);
					}

					free(text_overlay);
					y_offset += icon_size + icon_spacing;
				}
			}

			// Draw volume widget tile if enabled
			if (app_config.show_volume) {
				int percent = 0, muted = 0;
				volume_get_info(&percent, &muted);

				char label[16] = {0};
				if (app_config.label_mode == LABEL_MODE_ALWAYS)
					volume_get_label(label, sizeof(label), percent, muted);

				draw_tile(volume_get_icon_path(percent, muted), label,
					app_config.count, is_vertical);

				y_offset += icon_size + icon_spacing;
			}

			// Draw date widget tile if enabled
			if (app_config.show_date) {
				int slot = get_date_slot_index();
				int offset = get_offset_for_icon(slot);
				int tile_w = app_config.date_tile_width > 0 ?
					app_config.date_tile_width :
					icon_size;
				int tile_h = icon_size;
				uint32_t *tile = malloc(tile_w * tile_h * 4);
				if (tile) {
					date_draw_tile(tile, tile_w, tile_h, &app_config);
					// Vertical bar: along-bar = Y, tile_w is the height of the
					// slot
					for (int ty = 0; ty < tile_w; ty++) {
						uint32_t *src = tile + ty * tile_h;
						uint32_t *dst = pixels + (offset + ty) * surf_width;
						memcpy(dst, src, tile_h * 4);
					}
					free(tile);
				}
			}
		} else {
			// Draw each application icon horizontally (left to right)
			int x_offset = 0;
			for (int i = 0; i < app_config.count; i++) {
				if (x_offset + icon_size > surf_width)
					break; // Don't draw beyond buffer width

				if (app_config.apps[i]->icon) {
					if (verbose >= 2) {
						printf(
							"[DBG²] Drawing icon %d at x=%d: %s (size=%dx%d)\n",
							i, x_offset, app_config.apps[i]->icon, icon_size,
							icon_size);
					}

					// Create a temporary buffer for this icon tile
					uint32_t *tile_data = malloc(icon_size * icon_size * 4);
					memset(tile_data, 0, icon_size * icon_size * 4);

					// Draw the SVG onto the tile buffer
					draw_icon(app_config.apps[i]->icon, tile_data, icon_size,
						icon_size);

					// Draw the app name as a text overlay on the icon
					uint32_t *text_overlay = malloc(icon_size * icon_size * 4);
					memcpy(text_overlay, tile_data, icon_size * icon_size * 4);

					free(tile_data);

					// Draw the label only when label_mode is ALWAYS
					if (app_config.label_mode == LABEL_MODE_ALWAYS) {
						int baseline = icon_size - app_config.label_offset;
						draw_text(text_overlay, icon_size, icon_size,
							app_config.apps[i]->name, baseline,
							app_config.label_color);
					}

					// Blit the text overlay onto the main buffer
					for (int ty = 0; ty < icon_size; ty++) {
						uint32_t *src = text_overlay + (ty * icon_size);
						uint32_t *dst = pixels + ((ty)*surf_width) + x_offset;
						memcpy(dst, src, icon_size * 4);
					}

					free(text_overlay);
					x_offset += icon_size + icon_spacing;
				}
			}

			// Draw volume widget tile if enabled
			if (app_config.show_volume) {
				int percent = 0, muted = 0;
				volume_get_info(&percent, &muted);

				char label[16] = {0};
				if (app_config.label_mode == LABEL_MODE_ALWAYS)
					volume_get_label(label, sizeof(label), percent, muted);

				draw_tile(volume_get_icon_path(percent, muted), label,
					app_config.count, is_vertical);

				x_offset += icon_size + icon_spacing;
			}

			// Draw date widget tile if enabled
			if (app_config.show_date) {
				int slot = get_date_slot_index();
				int offset = get_offset_for_icon(slot);
				int tile_w = app_config.date_tile_width > 0 ?
					app_config.date_tile_width :
					icon_size;
				int tile_h = icon_size;
				uint32_t *tile = malloc(tile_w * tile_h * 4);
				if (tile) {
					date_draw_tile(tile, tile_w, tile_h, &app_config);
					// Horizontal bar: along-bar = X, tile_w is the width of the
					// slot
					for (int ty = 0; ty < tile_h; ty++) {
						uint32_t *src = tile + ty * tile_w;
						uint32_t *dst = pixels + ty * surf_width + offset;
						memcpy(dst, src, tile_w * 4);
					}
					free(tile);
				}
			}
		}

		wl_surface_attach(surface, buffer, 0, 0);
		wl_surface_damage(surface, 0, 0, surf_width, surf_height);
	}

	// Acknowledge the configure before committing, as required by the
	// protocol
	zwlr_layer_surface_v1_ack_configure(surf, serial);
	wl_surface_commit(surface);
}

// Called when the compositor wants to close the layer surface (e.g. output
// being destroyed). Clean shutdown would unmap and destroy resources here;
// for now we just exit.
static void
layer_closed(void *data, struct zwlr_layer_surface_v1 *surf)
{
	fprintf(stderr, "Layer surface closed by compositor — exiting\n");
	exit(0);
}

// Seat and pointer listeners moved to seat.c

static const struct zwlr_layer_surface_v1_listener layer_listener = {
	.configure = layer_configure,
	.closed = layer_closed,
};

// ---------------------------------------------------------------------------
// Registry listener — binds Wayland globals as the compositor announces them
// ---------------------------------------------------------------------------
static void
registry_add(void *data, struct wl_registry *reg, uint32_t name,
	const char *interface, uint32_t version)
{
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		layer_shell =
			wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
		wl_seat_add_listener(seat, get_seat_listener(), NULL);
		if (verbose >= 2)
			printf("[DBG²] Bound to wl_seat\n");
	}
}

static void
registry_remove(void *data, struct wl_registry *reg, uint32_t name)
{
	// Handle removal of global objects if needed
	// For now, we don't actively track removals since globals are
	// considered relatively permanent for a single-bar application
	if (verbose >= 2)
		printf("[DBG²] Global object removed: name=%u\n", name);
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_add,
	.global_remove = registry_remove,
};

int
main(int argc, char *argv[])
{
	// Apply the user's locale so strftime() produces localised day/month names
	setlocale(LC_TIME, "");

	// Parse command-line arguments
	int opt;
	static struct option long_options[] = {{"verbose", no_argument, 0, 'v'},
		{"version", no_argument, 0, 'V'}, {0, 0, 0, 0}};

	while ((opt = getopt_long(argc, argv, "vV", long_options, NULL)) != -1) {
		switch (opt) {
		case 'v':
			verbose++;
			break;
		case 'V':
			printf("%s\n", VERSION);
			return 0;
		default:
			fprintf(stderr, "Usage: %s [-v|--verbose] [-V|--version]\n",
				argv[0]);
			return 1;
		}
	}

	// Initialize the surface cache
	cache_init();

	// Load config (will create it if it doesn't exist)
	app_config = load_config();
	if (!app_config.apps || app_config.count == 0) {
		fprintf(stderr, "Failed to load configuration...\n");
		cache_free();
		return 1;
	}

	// Compute the date widget tile size now that font sizes are known.
	// This must happen before any layout or bar-size calculation.
	if (app_config.show_date)
		app_config.date_tile_width = date_compute_tile_size(&app_config);

	if (verbose) {
		printf("[DBG] Loaded configuration with %d application(s):\n",
			app_config.count);
		for (int i = 0; i < app_config.count; i++) {
			printf("[DBG]   #%d %s\n", i, app_config.apps[i]->name);
			if (app_config.apps[i]->icon)
				printf("[DBG]       icon: %s\n", app_config.apps[i]->icon);
			if (app_config.apps[i]->terminal)
				printf("[DBG]       terminal: true\n");
			printf("[DBG]       exec: %s\n", app_config.apps[i]->exec);
		}
		if (app_config.show_date)
			printf("[DBG] Date widget enabled (date: '%s' / time: '%s')\n",
				app_config.date_date_format ? app_config.date_date_format :
											  "%a %d",
				app_config.date_time_format ? app_config.date_time_format :
											  "%H:%M");
	}

	// Connect to the Wayland compositor
	display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "Failed to connect to Wayland display\n");
		free_config(&app_config);
		return 1;
	}

	if (verbose >= 2)
		printf("[DBG²] Connected to Wayland display\n");

	// Discover and bind compositor globals (wl_compositor, wl_shm,
	// layer-shell)
	registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(
		display); // Block until the registry is fully populated

	// Validate that all required globals were found
	if (!compositor || !shm || !layer_shell) {
		fprintf(stderr,
			"Missing required Wayland globals "
			"(compositor=%p shm=%p layer_shell=%p)\n",
			(void *)compositor, (void *)shm, (void *)layer_shell);
		return 1;
	}

	// Create a plain Wayland surface to host our layer surface
	surface = wl_compositor_create_surface(compositor);

	// Calculate bar dimensions.
	// Regular slots each occupy icon_size; the date slot uses date_tile_width.
	int total_count = get_total_widget_count();
	int date_slot_idx = get_date_slot_index();
	int icon_span = 0;
	for (int i = 0; i < total_count; i++) {
		if (i > 0)
			icon_span += app_config.icon_spacing;
		icon_span += (i == date_slot_idx && app_config.date_tile_width > 0) ?
			app_config.date_tile_width :
			app_config.icon_size;
	}
	// The bar cross-dimension (height for horizontal bar, width for vertical)
	// is always icon_size — font size only affects the slot's width, not the
	// bar's thickness.
	int cross_size = app_config.icon_size;

	if (verbose) {
		printf("[DBG] Bar dimensions: %d slot(s), span=%d px, cross=%d px\n",
			total_count, icon_span, cross_size);
	}

	// Map the Config layer enum to the Wayland layer-shell layer value
	uint32_t wl_layer = ZWLR_LAYER_SHELL_V1_LAYER_TOP; // Default
	switch (app_config.layer) {
	case LAYER_BACKGROUND:
		wl_layer = ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
		break;
	case LAYER_BOTTOM:
		wl_layer = ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM;
		break;
	case LAYER_OVERLAY:
		wl_layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
		break;
	case LAYER_TOP:
	default:
		wl_layer = ZWLR_LAYER_SHELL_V1_LAYER_TOP;
		break;
	}

	if (verbose) {
		const char *layer_name = "unknown";
		switch (app_config.layer) {
		case LAYER_OVERLAY:
			layer_name = "overlay";
			break;
		case LAYER_TOP:
			layer_name = "top";
			break;
		case LAYER_BACKGROUND:
			layer_name = "background";
			break;
		case LAYER_BOTTOM:
			layer_name = "bottom";
			break;
		}
		printf("[DBG] Using layer-shell layer: %s\n", layer_name);
	}

	// Promote the surface to a layer-shell surface using the configured layer.
	// The namespace "labar" identifies our client to the compositor.
	layer_surface = zwlr_layer_shell_v1_get_layer_surface(layer_shell, surface,
		NULL, wl_layer, "labar");

	// Set the layer for the surface
	zwlr_layer_surface_v1_set_layer(layer_surface, wl_layer);

	// Anchor the bar and set dimensions based on position
	uint32_t anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM; // Default anchor
	int bar_width = icon_span;
	int bar_height_actual = cross_size;

	switch (app_config.position) {
	case POSITION_TOP:
		anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
		bar_width = icon_span;
		bar_height_actual = cross_size;
		break;
	case POSITION_LEFT:
		anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
		bar_width = cross_size;
		bar_height_actual = icon_span;
		break;
	case POSITION_RIGHT:
		anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
		bar_width = cross_size;
		bar_height_actual = icon_span;
		break;
	case POSITION_BOTTOM:
	default:
		anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
		bar_width = icon_span;
		bar_height_actual = cross_size;
		break;
	}

	zwlr_layer_surface_v1_set_anchor(layer_surface, anchor);

	// Request the calculated bar dimensions
	zwlr_layer_surface_v1_set_size(layer_surface, bar_width, bar_height_actual);

	// Reserve space so other surfaces don't overlap the dock
	zwlr_layer_surface_v1_set_exclusive_zone(layer_surface,
		app_config.exclusive_zone);

	zwlr_layer_surface_v1_add_listener(layer_surface, &layer_listener, NULL);

	// Initial commit triggers the configure event from the compositor
	wl_surface_commit(surface);

	// Persistent state for the date widget repaint timer.
	// Initialised to -1 so the first call to date_widget_needs_repaint()
	// always triggers an immediate repaint.
	int date_last_minute = -1;
	int date_last_second = -1;

	// Determine the required poll timeout from the configured time format.
	//
	// If the format contains a seconds-level directive (%S, %T, %X, %c, %r)
	// we must refresh every second.  Otherwise we only need to wake up once
	// per minute, so we sleep until the next minute boundary (≤ 60 s) to
	// avoid unnecessary wakeups.
	//
	// A NULL / empty format falls back to the default "%H:%M" (minute-level).
	const char *time_fmt =
		(app_config.show_date && app_config.date_time_format &&
			app_config.date_time_format[0]) ?
		app_config.date_time_format :
		WIDGET_DATE_TIME_FORMAT;

	int needs_seconds = (strstr(time_fmt, "%S") || strstr(time_fmt, "%T") ||
		strstr(time_fmt, "%X") || strstr(time_fmt, "%c") ||
		strstr(time_fmt, "%r"));

	if (verbose && app_config.show_date)
		printf("[DBG] Date widget refresh: %s\n",
			needs_seconds ? "every second" : "every minute");

	// Main event loop.
	//
	// We need to repaint the date tile independently of Wayland input events.
	// wl_display_dispatch() blocks until an event arrives, so we use a
	// poll()-based loop instead:
	//
	//   1. Flush outgoing messages.
	//   2. Dispatch already-queued incoming events (non-blocking).
	//   3. Check whether the date tile needs a repaint.
	//   4. poll() on the Wayland fd with a timeout calibrated to the next
	//      expected change in the displayed time string:
	//        - seconds format → 200 ms  (snappy, handles sub-second drift)
	//        - minutes format → ms until the next whole minute + 50 ms slop
	struct pollfd pfd = {
		.fd = wl_display_get_fd(display),
		.events = POLLIN,
	};

	while (1) {
		// 1. Flush pending outgoing messages
		if (wl_display_flush(display) < 0)
			break;

		// 2. Dispatch all already-queued events without blocking
		if (wl_display_dispatch_pending(display) < 0)
			break;

		// 3. Repaint date tile if the displayed string has changed
		if (app_config.show_date) {
			int needs_repaint = needs_seconds ?
				date_widget_needs_repaint_seconds(&date_last_second) :
				date_widget_needs_repaint(&date_last_minute);
			if (needs_repaint) {
				date_repaint_tile(surface);
				// Flush immediately — the repaint queues wl_surface_commit
				// which must reach the compositor now, not on the next
				// iteration (which may be up to a minute away).
				wl_display_flush(display);
			}
		}

		// 4. Compute the timeout for this iteration
		int timeout_ms;
		if (!app_config.show_date) {
			// No date widget — block indefinitely on Wayland events
			timeout_ms = -1;
		} else if (needs_seconds) {
			// Wake up every 200 ms so we never miss a second tick
			timeout_ms = 200;
		} else {
			// Sleep until 50 ms after the next whole minute
			struct timespec ts;
			clock_gettime(CLOCK_REALTIME, &ts);
			int secs_into_minute = (int)(ts.tv_sec % 60);
			int secs_remaining = 60 - secs_into_minute;
			int ms_remaining = secs_remaining * 1000 -
				(int)(ts.tv_nsec / 1000000) + 50; /* slop */
			timeout_ms = ms_remaining > 0 ? ms_remaining : 50;
		}

		int ret = poll(&pfd, 1, timeout_ms);
		if (ret < 0)
			break;
		if (ret > 0) {
			// Data available — read and dispatch it
			if (wl_display_dispatch(display) < 0)
				break;
		}
		// ret == 0 → timeout, loop back to repaint and recalculate
	}

	cache_free();
	free_config(&app_config);
	wl_display_disconnect(display);
	return 0;
}
