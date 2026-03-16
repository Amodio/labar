#define _GNU_SOURCE // Required for memfd_create()

#include <cairo.h>
#include <fcntl.h>
#include <getopt.h>
#include <librsvg/rsvg.h>
#include <linux/input-event-codes.h>
#include <locale.h>
#include <math.h>
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
#ifdef HAVE_GTK4
#include "config-window.h"
#endif
#include "exec.h"
#include "seat.h"
#include "widget-date.h"
#include "widget-volume.h"
#include "wlr-layer-shell-unstable-v1-protocol.h"

// wp_fractional_scale_v1 + wp_viewporter for sub-integer HiDPI.
#include "fractional-scale-v1-protocol.h"
#include "viewporter-protocol.h"

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
struct wl_output *output; // First output — used to read buffer scale

// ---------------------------------------------------------------------------
// HiDPI scale state
//
// Integer scaling (wl_output.scale): buffer_scale is the integer multiplier.
// Fractional scaling (wp_fractional_scale_v1): scale is expressed as a
//   fixed-point value with 1/120 precision.  We store both and pick whichever
//   is available; fractional takes precedence when the extension is present.
//
// All physical pixel calculations use buffer_scale_num / buffer_scale_denom
// as the effective ratio (so integer 2× is represented as 240/120).
// The integer buffer_scale is used for wl_surface_set_buffer_scale() which
// only accepts integers; fractional mode uses wp_viewport instead.
// ---------------------------------------------------------------------------
int buffer_scale = 1;			// Integer scale (wl_output.scale)
double buffer_scale_frac = 1.0; // Effective scale including fractional

struct wp_fractional_scale_manager_v1 *fractional_scale_manager = NULL;
struct wp_fractional_scale_v1 *fractional_scale_obj = NULL;
struct wp_viewporter *viewporter = NULL;
struct wp_viewport *viewport_obj = NULL;
int using_fractional_scale = 0; // 1 when fractional protocol is active

// ---------------------------------------------------------------------------
// Layer-surface state
// ---------------------------------------------------------------------------
struct wl_surface *surface;
struct zwlr_layer_surface_v1 *layer_surface;
struct wl_buffer *buffer;
uint32_t *pixels; // Pointer into the SHM mapping

// Surface dimensions in LOGICAL pixels — updated on configure events.
// The compositor always talks in logical pixels; we multiply by buffer_scale
// when allocating SHM buffers and blitting tiles.
int surf_width = 64;
int surf_height = 64;

// Physical pixel dimensions of the SHM buffer (surf_* × buffer_scale).
int phys_width = 64;
int phys_height = 64;

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
	cairo_set_font_size(cr, (double)app_config.label_size * buffer_scale);

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
	int icon_size = app_config.icon_size * buffer_scale;
	int offset = get_offset_for_icon(slot_index) * buffer_scale;

	uint32_t *tile = malloc(icon_size * icon_size * 4);
	if (!tile)
		return;
	memset(tile, 0, icon_size * icon_size * 4);

	draw_icon(icon_path, tile, icon_size, icon_size);

	if (label && label[0] != '\0') {
		int baseline = icon_size - app_config.label_offset * buffer_scale;
		draw_text(tile, icon_size, icon_size, label, baseline,
			app_config.label_color);
	}

	if (is_vertical) {
		for (int ty = 0; ty < icon_size; ty++) {
			uint32_t *src = tile + ty * icon_size;
			uint32_t *dst = pixels + (offset + ty) * phys_width;
			memcpy(dst, src, icon_size * 4);
		}
	} else {
		for (int ty = 0; ty < icon_size; ty++) {
			uint32_t *src = tile + ty * icon_size;
			uint32_t *dst = pixels + ty * phys_width + offset;
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
	int offset = get_offset_for_icon(slot) * buffer_scale;
	int icon_size = app_config.icon_size * buffer_scale;
	// Along-bar dimension may be wider than icon_size to fit the text;
	// the cross-bar dimension is always icon_size.
	int tile_w = (app_config.date_tile_width > 0 ? app_config.date_tile_width :
												   app_config.icon_size) *
		buffer_scale;
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
			uint32_t *dst = pixels + (offset + ty) * phys_width;
			memcpy(dst, src, tile_h * 4);
		}
	} else {
		// Horizontal bar: offset is along X; tile_w is the width, tile_h is the
		// height
		for (int ty = 0; ty < tile_h; ty++) {
			uint32_t *src = tile + ty * tile_w;
			uint32_t *dst = pixels + ty * phys_width + offset;
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
	// Compositor provides LOGICAL dimensions; we allocate a physical buffer
	// that is buffer_scale times larger in each dimension.
	// For fractional scaling the physical buffer is ceil(scale) × logical,
	// and wp_viewport maps it back to the exact logical size.
	surf_width = width > 0 ? (int)width : 64;
	surf_height = height > 0 ? (int)height : 64;
	// Use the integer ceiling of the effective scale for SHM allocation.
	int phys_scale = buffer_scale; // already ceil'd in fractional path
	phys_width = surf_width * phys_scale;
	phys_height = surf_height * phys_scale;

	if (!buffer) {
		// Allocate SHM buffer at physical (HiDPI) resolution
		buffer = create_shm_buffer(phys_width, phys_height, &pixels);

		// Clear the entire buffer to transparent
		memset(pixels, 0, phys_width * phys_height * 4);

		// Physical icon and spacing sizes
		int icon_size = app_config.icon_size * buffer_scale;
		int icon_spacing = app_config.icon_spacing * buffer_scale;

		// Determine if bar is horizontal or vertical
		int is_vertical = (app_config.position == POSITION_LEFT ||
			app_config.position == POSITION_RIGHT);

		if (is_vertical) {
			// Draw each application icon vertically (top to bottom)
			int y_offset = 0;
			for (int i = 0; i < app_config.count; i++) {
				if (y_offset + icon_size > phys_height)
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
						int baseline =
							icon_size - app_config.label_offset * buffer_scale;
						draw_text(text_overlay, icon_size, icon_size,
							app_config.apps[i]->name, baseline,
							app_config.label_color);
					}

					// Blit the text overlay onto the main buffer
					for (int ty = 0; ty < icon_size; ty++) {
						uint32_t *src = text_overlay + (ty * icon_size);
						uint32_t *dst = pixels + ((y_offset + ty) * phys_width);
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
				int offset = get_offset_for_icon(slot) * buffer_scale;
				int tile_w = (app_config.date_tile_width > 0 ?
									 app_config.date_tile_width :
									 app_config.icon_size) *
					buffer_scale;
				int tile_h = icon_size;
				uint32_t *tile = malloc(tile_w * tile_h * 4);
				if (tile) {
					date_draw_tile(tile, tile_w, tile_h, &app_config);
					// Vertical bar: along-bar = Y, tile_w is the height of the
					// slot
					for (int ty = 0; ty < tile_w; ty++) {
						uint32_t *src = tile + ty * tile_h;
						uint32_t *dst = pixels + (offset + ty) * phys_width;
						memcpy(dst, src, tile_h * 4);
					}
					free(tile);
				}
			}
		} else {
			// Draw each application icon horizontally (left to right)
			int x_offset = 0;
			for (int i = 0; i < app_config.count; i++) {
				if (x_offset + icon_size > phys_width)
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
						int baseline =
							icon_size - app_config.label_offset * buffer_scale;
						draw_text(text_overlay, icon_size, icon_size,
							app_config.apps[i]->name, baseline,
							app_config.label_color);
					}

					// Blit the text overlay onto the main buffer
					for (int ty = 0; ty < icon_size; ty++) {
						uint32_t *src = text_overlay + (ty * icon_size);
						uint32_t *dst = pixels + (ty * phys_width) + x_offset;
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
				int offset = get_offset_for_icon(slot) * buffer_scale;
				int tile_w = (app_config.date_tile_width > 0 ?
									 app_config.date_tile_width :
									 app_config.icon_size) *
					buffer_scale;
				int tile_h = icon_size;
				uint32_t *tile = malloc(tile_w * tile_h * 4);
				if (tile) {
					date_draw_tile(tile, tile_w, tile_h, &app_config);
					// Horizontal bar: along-bar = X, tile_w is the width of the
					// slot
					for (int ty = 0; ty < tile_h; ty++) {
						uint32_t *src = tile + ty * tile_w;
						uint32_t *dst = pixels + ty * phys_width + offset;
						memcpy(dst, src, tile_w * 4);
					}
					free(tile);
				}
			}
		}

		// Tell the compositor how to interpret the buffer pixels.
		// - Integer scaling: wl_surface_set_buffer_scale(scale)
		// - Fractional scaling: wp_viewport maps the oversized buffer
		//   back to the exact logical surface size.
		if (using_fractional_scale && viewport_obj) {
			// Buffer is ceil(scale) × logical size; viewport corrects it.
			wp_viewport_set_destination(viewport_obj, surf_width, surf_height);
		} else {
			wl_surface_set_buffer_scale(surface, buffer_scale);
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
// wl_output listener — reads the display's buffer scale for HiDPI
// ---------------------------------------------------------------------------
static void
output_geometry(void *data, struct wl_output *wl_output, int32_t x, int32_t y,
	int32_t physical_width, int32_t physical_height, int32_t subpixel,
	const char *make, const char *model, int32_t transform)
{
	// Not needed for scale detection
}

static void
output_mode(void *data, struct wl_output *wl_output, uint32_t flags,
	int32_t width, int32_t height, int32_t refresh)
{
	// Not needed for scale detection
}

static void
output_done(void *data, struct wl_output *wl_output)
{
	// All output properties delivered; buffer_scale is already applied
}

// ---------------------------------------------------------------------------
// trigger_redraw
//
// Force a full re-render at the current buffer_scale.  Used when the scale
// changes due to output hotplug or fractional scale notification.
//
// Strategy: destroy the current wl_buffer so that the next layer_configure
// callback (re-triggered by resizing the layer surface to its own size)
// allocates a fresh one at the new physical resolution.
// ---------------------------------------------------------------------------
static void
trigger_redraw(void)
{
	if (verbose)
		printf("[DBG] trigger_redraw: scale=%.6f (integer %d)\n",
			buffer_scale_frac, buffer_scale);

	// Invalidate the icon cache — entries were rendered at the old scale.
	cache_clear();

	// Recompute the date tile width at the new scale.
	if (app_config.show_date) {
		int phys_w = date_compute_tile_size(&app_config);
		app_config.date_tile_width = (buffer_scale > 1) ?
			(phys_w + buffer_scale - 1) / buffer_scale :
			phys_w;
	}

	// Destroy the stale buffer; layer_configure will create a new one.
	if (buffer) {
		wl_buffer_destroy(buffer);
		buffer = NULL;
		pixels = NULL;
	}

	// Re-trigger layer_configure by setting the same size again.
	if (layer_surface && surf_width > 0 && surf_height > 0) {
		zwlr_layer_surface_v1_set_size(layer_surface, surf_width, surf_height);
		wl_surface_commit(surface);
	}
}

static void
output_scale(void *data, struct wl_output *wl_output, int32_t factor)
{
	// When fractional scale protocol is active, ignore integer scale events
	// — the fractional listener is authoritative.
	if (using_fractional_scale)
		return;

	if (factor >= 1) {
		int old = buffer_scale;
		buffer_scale = factor;
		buffer_scale_frac = (double)factor;
		if (verbose)
			printf("[DBG] Output scale: %d\n", buffer_scale);
		// If the scale changed after the initial setup, force a full redraw.
		if (old != buffer_scale && buffer)
			trigger_redraw();
	}
}

static const struct wl_output_listener output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
};

// ---------------------------------------------------------------------------
// wl_surface enter/leave — detect when the bar moves to a different output
// ---------------------------------------------------------------------------
static void
surface_enter(void *data, struct wl_surface *surf, struct wl_output *entered)
{
	if (verbose >= 2)
		printf("[DBG²] Surface entered output %p\n", (void *)entered);

	// If this is a different output than the one we tracked, re-read its
	// scale and trigger a redraw if it changed.
	if (entered && entered != output) {
		// Rebind our output pointer; the old listener is still valid
		// because we never destroy it, but scale events going forward will
		// come from the new output through its own listener if we add one.
		// For simplicity: request a new roundtrip so pending scale events
		// from this output are processed, then check if scale changed.
		// (A full multi-output tracking system would require maintaining a
		//  list — this handles the common single-bar-moves-to-new-monitor
		//  case.)
		output = entered;
		wl_output_add_listener(output, &output_listener, NULL);
	}
}

static void
surface_leave(void *data, struct wl_surface *surf, struct wl_output *left)
{
	if (verbose >= 2)
		printf("[DBG²] Surface left output %p\n", (void *)left);
}

static const struct wl_surface_listener surface_listener = {
	.enter = surface_enter,
	.leave = surface_leave,
};

// ---------------------------------------------------------------------------
// wp_fractional_scale_v1 listener
// ---------------------------------------------------------------------------
static void
fractional_scale_preferred(void *data, struct wp_fractional_scale_v1 *obj,
	uint32_t scale_120)
{
	// scale_120 is the scale multiplied by 120 (e.g. 150% → 180, 200% → 240)
	double new_scale = scale_120 / 120.0;
	if (verbose)
		printf("[DBG] Fractional scale: %u/120 = %.4f\n", scale_120, new_scale);

	if (fabs(new_scale - buffer_scale_frac) < 1e-6)
		return; // No change

	buffer_scale_frac = new_scale;
	// For SHM buffer allocation we ceil to an integer; wp_viewport
	// will correct the mapping back to the exact fractional size.
	buffer_scale = (int)ceil(new_scale);
	using_fractional_scale = 1;
	trigger_redraw();
}

static const struct wp_fractional_scale_v1_listener fractional_scale_listener =
	{
		.preferred_scale = fractional_scale_preferred,
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
	} else if (strcmp(interface, wl_output_interface.name) == 0 && !output) {
		// Bind the first output; wl_output v2 is needed for scale events.
		// Additional outputs are handled via wl_surface.enter events.
		output = wl_registry_bind(reg, name, &wl_output_interface, 2);
		wl_output_add_listener(output, &output_listener, NULL);
		if (verbose >= 2)
			printf("[DBG²] Bound to wl_output\n");
	} else if (strcmp(interface,
				   wp_fractional_scale_manager_v1_interface.name) == 0) {
		fractional_scale_manager = wl_registry_bind(reg, name,
			&wp_fractional_scale_manager_v1_interface, 1);
		if (verbose >= 2)
			printf("[DBG²] Bound to wp_fractional_scale_manager_v1\n");
	} else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
		viewporter = wl_registry_bind(reg, name, &wp_viewporter_interface, 1);
		if (verbose >= 2)
			printf("[DBG²] Bound to wp_viewporter\n");
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
	static struct option long_options[] = {{"help", no_argument, 0, 'h'},
		{"verbose", no_argument, 0, 'v'}, {"version", no_argument, 0, 'V'},
		{"config", no_argument, 0, 'c'}, {0, 0, 0, 0}};

	while ((opt = getopt_long(argc, argv, "hvVc", long_options, NULL)) != -1) {
		switch (opt) {
		case 'h':
			printf(
				"Usage: %s [OPTIONS]\n"
				"\n"
				"Options:\n"
				"  -h, --help       Show this help message and exit\n"
				"  -v, --verbose    Increase verbosity (use up to 4 times)\n"
				"  -V, --version    Print version and exit\n"
				"  -c, --config     Open the graphical configuration window\n"
				"\nlabar reads its configuration from:\n"
				"  ~/.config/labar/labar.cfg\n",
				argv[0]);
			return 0;
		case 'v':
			verbose++;
			break;
		case 'V':
			printf("%s\n", VERSION);
			return 0;
#ifdef HAVE_GTK4
		case 'c':
			return config_window_run();
#else
		case 'c':
			fprintf(stderr,
				"labar was built without GTK4; --config is not available.\n");
			return 1;
#endif
		default:
			fprintf(stderr,
				"Usage: %s [-h|--help] [-v|--verbose] [-V|--version] [-c|--config]\n",
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
	wl_display_roundtrip(display); // Block until registry is fully populated

	// A second roundtrip receives wl_output.scale (and .done) events so that
	// buffer_scale is correctly set before any SHM buffer is allocated.
	wl_display_roundtrip(display);

	// Compute the date widget tile width now that buffer_scale is known.
	// date_compute_tile_size() returns a physical pixel count (it scales font
	// sizes by buffer_scale internally). We divide back to logical pixels so
	// that all layout helpers (get_offset_for_icon, icon_span, etc.) can
	// multiply by buffer_scale uniformly at draw time — consistent with every
	// other dimension in the codebase.
	if (app_config.show_date) {
		int phys_w = date_compute_tile_size(&app_config);
		app_config.date_tile_width = (buffer_scale > 1) ?
			(phys_w + buffer_scale - 1) / buffer_scale :
			phys_w;
	}

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

	// Listen for enter/leave events so we can detect output changes
	wl_surface_add_listener(surface, &surface_listener, NULL);

	// If both wp_fractional_scale_manager_v1 and wp_viewporter are available,
	// set up fractional scaling.  The preferred_scale callback will fire before
	// the first configure and set buffer_scale/buffer_scale_frac correctly.
	if (fractional_scale_manager && viewporter) {
		fractional_scale_obj =
			wp_fractional_scale_manager_v1_get_fractional_scale(
				fractional_scale_manager, surface);
		wp_fractional_scale_v1_add_listener(fractional_scale_obj,
			&fractional_scale_listener, NULL);
		viewport_obj = wp_viewporter_get_viewport(viewporter, surface);
		if (verbose)
			printf("[DBG] Fractional scale + viewport objects created\n");
	} else {
		if (verbose)
			printf(
				"[DBG] Fractional scale not available; using integer scale\n");
	}

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
