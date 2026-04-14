#define _GNU_SOURCE // Required for memfd_create()

#include <cairo.h>
#include <fcntl.h>
#include <getopt.h>
#include <librsvg/rsvg.h>
#include <linux/input-event-codes.h>
#include <locale.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
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
#include <dlfcn.h>
#include "config-window.h"
#endif
#include "calendar-popup.h"
#include "exec.h"
#include "seat.h"
#include "widget-date.h"
#include "widget-net.h"
#include "widget-sysinfo.h"
#include "widget-volume.h"
#include "wlr-layer-shell-unstable-v1-protocol.h"

// wp_fractional_scale_v1 + wp_viewporter for sub-integer HiDPI.
#include "fractional-scale-v1-protocol.h"
#include "viewporter-protocol.h"

// zxdg_output_manager_v1 for logical output geometry and output names.
#include "xdg-output-unstable-v1-protocol.h"

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
// xdg-output-unstable-v1 — logical geometry and output names
// ---------------------------------------------------------------------------
struct zxdg_output_manager_v1 *xdg_output_manager = NULL;

// Per-output info populated by the zxdg_output_v1 listener.
// We track up to 8 outputs; more than that is extremely unusual.
#define MAX_OUTPUTS 8
static struct output_info {
	struct wl_output *wl_out;
	struct zxdg_output_v1 *xdg_out;
	char name[64];
	char description[128];
	int32_t logical_x, logical_y;
	int32_t logical_w, logical_h;
	uint32_t registry_name; // Wayland global name for removal
} outputs[MAX_OUTPUTS];
static int n_outputs = 0;

static void
handle_sigterm(int sig)
{
	(void)sig;
	exit(0); // triggers LSan via atexit hooks
}

// ---------------------------------------------------------------------------
// Layer-surface state
// ---------------------------------------------------------------------------
struct wl_surface *surface;
struct zwlr_layer_surface_v1 *layer_surface;
struct wl_buffer *buffer;
uint32_t *pixels;	// Pointer into the SHM mapping
size_t pixels_size; // Size of the SHM mapping in bytes (for munmap)

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
// Widget ID constants — must match widget_order[] values in config.h
// ---------------------------------------------------------------------------
#define WIDGET_ID_SYSINFO 0
#define WIDGET_ID_NET 1
#define WIDGET_ID_APPS 2 // sentinel: marks the app-icons block position
#define WIDGET_ID_VOLUME 3
#define WIDGET_ID_DATE 4

static int
widget_enabled(int id)
{
	switch (id) {
	case WIDGET_ID_NET:
		return app_config.show_net;
	case WIDGET_ID_VOLUME:
		return app_config.show_volume;
	case WIDGET_ID_DATE:
		return app_config.show_date;
	case WIDGET_ID_APPS:
		return app_config.count > 0;
	case WIDGET_ID_SYSINFO:
		return app_config.show_sysinfo;
	default:
		return 0;
	}
}

// ---------------------------------------------------------------------------
// get_total_widget_count
// ---------------------------------------------------------------------------
static int
get_total_widget_count(void)
{
	int n = app_config.count;
	for (int i = 0; i < 5; i++)
		if (app_config.widget_order[i] != WIDGET_ID_APPS &&
			widget_enabled(app_config.widget_order[i]))
			n++;
	return n;
}

// ---------------------------------------------------------------------------
// slot_index_for_widget
//
// Walks widget_order[5].  The WIDGET_ID_APPS entry marks where the app block
// sits.  Widgets before it are pre-app; widgets after it are post-app.
// ---------------------------------------------------------------------------
static int
slot_index_for_widget(int id)
{
	if (!widget_enabled(id))
		return -1;

	int pre = 0;  // enabled widgets before the apps block
	int post = 0; // enabled widgets between apps block and id
	int past_apps = 0;

	for (int i = 0; i < 5; i++) {
		int wid = app_config.widget_order[i];
		if (wid == WIDGET_ID_APPS) {
			past_apps = 1;
			continue;
		}
		if (wid == id) {
			if (!past_apps)
				return pre; // pre-app slot
			else
				return pre + app_config.count + post; // post-app slot
		}
		if (!widget_enabled(wid))
			continue;
		if (!past_apps)
			pre++;
		else
			post++;
	}
	return -1; // not found
}

int
get_net_slot_index(void)
{
	return slot_index_for_widget(WIDGET_ID_NET);
}

int
get_volume_slot_index(void)
{
	return slot_index_for_widget(WIDGET_ID_VOLUME);
}

int
get_date_slot_index(void)
{
	return slot_index_for_widget(WIDGET_ID_DATE);
}

int
get_sysinfo_slot_index(void)
{
	return slot_index_for_widget(WIDGET_ID_SYSINFO);
}

// ---------------------------------------------------------------------------
// get_app_first_slot
//
// Returns the slot index of the first app icon: the count of enabled widgets
// that appear before the WIDGET_ID_APPS entry in widget_order.
// ---------------------------------------------------------------------------
int
get_app_first_slot(void)
{
	int pre = 0;
	for (int i = 0; i < 5; i++) {
		int wid = app_config.widget_order[i];
		if (wid == WIDGET_ID_APPS)
			break;
		if (widget_enabled(wid))
			pre++;
	}
	return pre;
}

// ---------------------------------------------------------------------------
// tile_has_bg / get_corner_flags
//
// Returns whether the widget occupying slot_index has a background colour,
// and computes the TILE_ROUND_LEFT / TILE_ROUND_RIGHT flags for that slot so
// that adjacent bg-enabled tiles share a unified pill shape.
// ---------------------------------------------------------------------------
static int
tile_has_bg(int slot_index)
{
	int date_slot = get_date_slot_index();
	int net_slot = get_net_slot_index();
	int sysinfo_slot = get_sysinfo_slot_index();
	if (slot_index == date_slot)
		return app_config.date_bg_color != 0;
	if (slot_index == net_slot)
		return app_config.net_bg_color != 0;
	if (slot_index == sysinfo_slot)
		return app_config.sysinfo_bg_color != 0;
	return 0;
}

int
get_corner_flags(int slot_index)
{
	int total = get_total_widget_count();
	int flags = TILE_ROUND_ALL;
	// If the previous slot also has a bg, don't round the left side
	if (slot_index > 0 && tile_has_bg(slot_index - 1))
		flags &= ~TILE_ROUND_LEFT;
	// If the next slot also has a bg, don't round the right side
	if (slot_index < total - 1 && tile_has_bg(slot_index + 1))
		flags &= ~TILE_ROUND_RIGHT;
	return flags;
}

// ---------------------------------------------------------------------------
// fill_bg_gaps
//
// After all tiles are drawn, fill the icon_spacing gap between every pair of
// adjacent bg-enabled slots so they look like a single unified pill.
// Uses the left tile's bg colour for the gap fill.
// ---------------------------------------------------------------------------
static void
fill_bg_gaps(int is_vertical, int icon_size_phys)
{
	if (!pixels || !buffer)
		return;
	int total = get_total_widget_count();
	int spacing = app_config.icon_spacing * buffer_scale;
	if (spacing <= 0)
		return;

	for (int i = 0; i < total - 1; i++) {
		if (!tile_has_bg(i) || !tile_has_bg(i + 1))
			continue;

		// Both adjacent slots have a background — fill the gap between them
		unsigned int bg_col = 0;
		int net_s = get_net_slot_index();
		int si_s = get_sysinfo_slot_index();
		int date_s = get_date_slot_index();
		if (i == net_s)
			bg_col = app_config.net_bg_color;
		else if (i == si_s)
			bg_col = app_config.sysinfo_bg_color;
		else if (i == date_s)
			bg_col = app_config.date_bg_color;
		if (!bg_col)
			continue;

		// Pixel position right after slot i ends
		int gap_start = (get_offset_for_icon(i + 1) - app_config.icon_spacing) *
			buffer_scale;
		if (gap_start < 0)
			gap_start = 0;

		double bg_a = ((bg_col >> 24) & 0xFF) / 255.0;
		double bg_r = ((bg_col >> 16) & 0xFF) / 255.0;
		double bg_g = ((bg_col >> 8) & 0xFF) / 255.0;
		double bg_b = ((bg_col) & 0xFF) / 255.0;
		uint32_t fill_px = ((uint8_t)(bg_a * 255) << 24) |
			((uint8_t)(bg_r * bg_a * 255) << 16) |
			((uint8_t)(bg_g * bg_a * 255) << 8) | (uint8_t)(bg_b * bg_a * 255);

		// Premultiplied alpha blend into existing pixels
		(void)fill_px; // use Cairo instead for correct alpha blend

		// Create a tiny Cairo surface over the gap region and paint it
		int surf_w = phys_width;
		int surf_h = icon_size_phys;
		cairo_surface_t *cs =
			cairo_image_surface_create_for_data((unsigned char *)pixels,
				CAIRO_FORMAT_ARGB32, surf_w, surf_h, surf_w * 4);
		cairo_t *cr = cairo_create(cs);
		cairo_set_source_rgba(cr, bg_r, bg_g, bg_b, bg_a);
		cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
		if (is_vertical)
			cairo_rectangle(cr, 0, gap_start, surf_w, spacing);
		else
			cairo_rectangle(cr, gap_start, 0, spacing, surf_h);
		cairo_fill(cr);
		cairo_destroy(cr);
		cairo_surface_destroy(cs);
	}
}

// ---------------------------------------------------------------------------
// get_slot_size
//
// Returns the along-bar pixel size for slot i.
// On horizontal bars, text-widget slots may be wider than icon_size.
// On vertical bars every slot is icon_size square — the text widgets render
// two lines inside the same square cell used by app icons.
// ---------------------------------------------------------------------------
int
get_slot_size(int slot_index, int is_vertical)
{
	if (is_vertical)
		return app_config.icon_size;

	int date_slot = get_date_slot_index();
	int net_slot = get_net_slot_index();
	int si_slot = get_sysinfo_slot_index();

	if (slot_index == date_slot && app_config.date_tile_width > 0)
		return app_config.date_tile_width;
	if (slot_index == net_slot && app_config.net_tile_width > 0)
		return app_config.net_tile_width;
	if (slot_index == si_slot && app_config.sysinfo_tile_width > 0)
		return app_config.sysinfo_tile_width;
	return app_config.icon_size;
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

	int is_vertical = (app_config.position == POSITION_LEFT ||
		app_config.position == POSITION_RIGHT);
	int spacing = app_config.icon_spacing;
	int total_count = get_total_widget_count();

	int pos = 0;
	for (int i = 0; i < total_count; i++) {
		int slot_size = get_slot_size(i, is_vertical);
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

	int is_vertical = (app_config.position == POSITION_LEFT ||
		app_config.position == POSITION_RIGHT);
	int spacing = app_config.icon_spacing;

	int pos = 0;
	for (int i = 0; i < icon_index; i++)
		pos += get_slot_size(i, is_vertical) + spacing;
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
create_shm_buffer(int width, int height, uint32_t **out_data, size_t *out_size)
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
	*out_size = size;

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
	// On vertical bars every slot is icon_size × icon_size (get_slot_size
	// returns icon_size).  On horizontal bars the tile may be wider.
	int along_px = get_slot_size(slot, is_vertical) * buffer_scale;
	int tile_w = is_vertical ? icon_size : along_px;
	int tile_h = icon_size;

	uint32_t *tile = malloc(tile_w * tile_h * 4);
	if (!tile)
		return;

	date_draw_tile(tile, tile_w, tile_h, &app_config, get_corner_flags(slot));

	if (is_vertical) {
		for (int ty = 0; ty < tile_h; ty++) {
			uint32_t *src = tile + ty * tile_w;
			uint32_t *dst = pixels + (offset + ty) * phys_width;
			memcpy(dst, src, tile_w * 4);
		}
	} else {
		for (int ty = 0; ty < tile_h; ty++) {
			uint32_t *src = tile + ty * tile_w;
			uint32_t *dst = pixels + ty * phys_width + offset;
			memcpy(dst, src, tile_w * 4);
		}
	}
	free(tile);

	fill_bg_gaps(is_vertical, icon_size);
	wl_surface_attach(wl_surf, buffer, 0, 0);
	wl_surface_damage(wl_surf, 0, 0, surf_width, surf_height);
	wl_surface_commit(wl_surf);
}

// ---------------------------------------------------------------------------
// net_repaint_tile
//
// Re-renders the network speed slot once per second.
// Called from the main dispatch loop via net_widget_needs_repaint().
// ---------------------------------------------------------------------------
static void
net_repaint_tile(struct wl_surface *wl_surf)
{
	if (!buffer || !app_config.show_net)
		return;

	int is_vertical = (app_config.position == POSITION_LEFT ||
		app_config.position == POSITION_RIGHT);
	int slot = get_net_slot_index();
	int offset = get_offset_for_icon(slot) * buffer_scale;
	int icon_size = app_config.icon_size * buffer_scale;
	int along_px = get_slot_size(slot, is_vertical) * buffer_scale;
	int tile_w = is_vertical ? icon_size : along_px;
	int tile_h = icon_size;

	uint32_t *tile = malloc(tile_w * tile_h * 4);
	if (!tile)
		return;

	net_draw_tile(tile, tile_w, tile_h, &app_config, get_corner_flags(slot));

	if (is_vertical) {
		for (int ty = 0; ty < tile_h; ty++) {
			uint32_t *src = tile + ty * tile_w;
			uint32_t *dst = pixels + (offset + ty) * phys_width;
			memcpy(dst, src, tile_w * 4);
		}
	} else {
		for (int ty = 0; ty < tile_h; ty++) {
			uint32_t *src = tile + ty * tile_w;
			uint32_t *dst = pixels + ty * phys_width + offset;
			memcpy(dst, src, tile_w * 4);
		}
	}
	free(tile);

	fill_bg_gaps(is_vertical, icon_size);

	wl_surface_attach(wl_surf, buffer, 0, 0);
	wl_surface_damage(wl_surf, 0, 0, surf_width, surf_height);
	wl_surface_commit(wl_surf);
}

// ---------------------------------------------------------------------------
// sysinfo_repaint_tile
// ---------------------------------------------------------------------------
static void
sysinfo_repaint_tile(struct wl_surface *wl_surf)
{
	if (!buffer || !app_config.show_sysinfo)
		return;

	int is_vertical = (app_config.position == POSITION_LEFT ||
		app_config.position == POSITION_RIGHT);
	int slot = get_sysinfo_slot_index();
	int offset = get_offset_for_icon(slot) * buffer_scale;
	int icon_size = app_config.icon_size * buffer_scale;
	int along_px = get_slot_size(slot, is_vertical) * buffer_scale;
	int tile_w = is_vertical ? icon_size : along_px;
	int tile_h = icon_size;

	uint32_t *tile = malloc(tile_w * tile_h * 4);
	if (!tile)
		return;

	sysinfo_draw_tile(tile, tile_w, tile_h, &app_config,
		get_corner_flags(slot));

	if (is_vertical) {
		for (int ty = 0; ty < tile_h; ty++) {
			uint32_t *src = tile + ty * tile_w;
			uint32_t *dst = pixels + (offset + ty) * phys_width;
			memcpy(dst, src, tile_w * 4);
		}
	} else {
		for (int ty = 0; ty < tile_h; ty++) {
			uint32_t *src = tile + ty * tile_w;
			uint32_t *dst = pixels + ty * phys_width + offset;
			memcpy(dst, src, tile_w * 4);
		}
	}
	free(tile);

	fill_bg_gaps(is_vertical, icon_size);

	wl_surface_attach(wl_surf, buffer, 0, 0);
	wl_surface_damage(wl_surf, 0, 0, surf_width, surf_height);
	wl_surface_commit(wl_surf);
}
// ---------------------------------------------------------------------------

// Called by the compositor when it assigns dimensions to our layer surface.
// This is where we create the SHM buffer and perform the first render.
static void
layer_configure(void *data, struct zwlr_layer_surface_v1 *surf, uint32_t serial,
	uint32_t width, uint32_t height)
{
	(void)data;
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
		buffer =
			create_shm_buffer(phys_width, phys_height, &pixels, &pixels_size);

		// Clear the entire buffer to transparent
		memset(pixels, 0, phys_width * phys_height * 4);

		// Physical icon size
		int icon_size = app_config.icon_size * buffer_scale;

		// Determine if bar is horizontal or vertical
		int is_vertical = (app_config.position == POSITION_LEFT ||
			app_config.position == POSITION_RIGHT);

		if (is_vertical) {
			// Draw each application icon vertically (top to bottom)
			int app_first_slot = get_app_first_slot();
			for (int i = 0; i < app_config.count; i++) {
				int y_offset =
					get_offset_for_icon(app_first_slot + i) * buffer_scale;
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

					// Draw the label only when label_mode is ALWAYS
					if (app_config.label_mode == LABEL_MODE_ALWAYS) {
						int baseline =
							icon_size - app_config.label_offset * buffer_scale;
						draw_text(tile_data, icon_size, icon_size,
							app_config.apps[i]->name, baseline,
							app_config.label_color);
					}

					// Blit the tile onto the main buffer
					for (int ty = 0; ty < icon_size; ty++) {
						uint32_t *src = tile_data + (ty * icon_size);
						uint32_t *dst = pixels + ((y_offset + ty) * phys_width);
						memcpy(dst, src, icon_size * 4);
					}

					free(tile_data);
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
					get_volume_slot_index(), is_vertical);
			}

			// Draw date widget tile if enabled
			if (app_config.show_date) {
				int slot = get_date_slot_index();
				int offset = get_offset_for_icon(slot) * buffer_scale;
				uint32_t *tile = malloc(icon_size * icon_size * 4);
				if (tile) {
					date_draw_tile(tile, icon_size, icon_size, &app_config,
						get_corner_flags(slot));
					for (int ty = 0; ty < icon_size; ty++) {
						uint32_t *src = tile + ty * icon_size;
						uint32_t *dst = pixels + (offset + ty) * phys_width;
						memcpy(dst, src, icon_size * 4);
					}
					free(tile);
				}
			}

			// Draw network widget tile if enabled
			if (app_config.show_net) {
				int slot = get_net_slot_index();
				int offset = get_offset_for_icon(slot) * buffer_scale;
				uint32_t *tile = malloc(icon_size * icon_size * 4);
				if (tile) {
					net_draw_tile(tile, icon_size, icon_size, &app_config,
						get_corner_flags(slot));
					for (int ty = 0; ty < icon_size; ty++) {
						uint32_t *src = tile + ty * icon_size;
						uint32_t *dst = pixels + (offset + ty) * phys_width;
						memcpy(dst, src, icon_size * 4);
					}
					free(tile);
				}
			}

			// Draw sysinfo widget tile if enabled
			if (app_config.show_sysinfo) {
				int slot = get_sysinfo_slot_index();
				int offset = get_offset_for_icon(slot) * buffer_scale;
				uint32_t *tile = malloc(icon_size * icon_size * 4);
				if (tile) {
					sysinfo_draw_tile(tile, icon_size, icon_size, &app_config,
						get_corner_flags(slot));
					for (int ty = 0; ty < icon_size; ty++) {
						uint32_t *src = tile + ty * icon_size;
						uint32_t *dst = pixels + (offset + ty) * phys_width;
						memcpy(dst, src, icon_size * 4);
					}
					free(tile);
				}
			}
		} else {
			// Draw each application icon horizontally (left to right)
			int app_first_slot = get_app_first_slot();
			for (int i = 0; i < app_config.count; i++) {
				int x_offset =
					get_offset_for_icon(app_first_slot + i) * buffer_scale;
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

					// Draw the label only when label_mode is ALWAYS
					if (app_config.label_mode == LABEL_MODE_ALWAYS) {
						int baseline =
							icon_size - app_config.label_offset * buffer_scale;
						draw_text(tile_data, icon_size, icon_size,
							app_config.apps[i]->name, baseline,
							app_config.label_color);
					}

					// Blit the tile onto the main buffer
					for (int ty = 0; ty < icon_size; ty++) {
						uint32_t *src = tile_data + (ty * icon_size);
						uint32_t *dst = pixels + (ty * phys_width) + x_offset;
						memcpy(dst, src, icon_size * 4);
					}

					free(tile_data);
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
					get_volume_slot_index(), is_vertical);
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
					date_draw_tile(tile, tile_w, tile_h, &app_config,
						get_corner_flags(slot));
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

			// Draw network widget tile if enabled
			if (app_config.show_net) {
				int slot = get_net_slot_index();
				int offset = get_offset_for_icon(slot) * buffer_scale;
				int tile_w =
					(app_config.net_tile_width > 0 ? app_config.net_tile_width :
													 app_config.icon_size) *
					buffer_scale;
				int tile_h = icon_size;
				uint32_t *tile = malloc(tile_w * tile_h * 4);
				if (tile) {
					net_draw_tile(tile, tile_w, tile_h, &app_config,
						get_corner_flags(slot));
					for (int ty = 0; ty < tile_h; ty++) {
						uint32_t *src = tile + ty * tile_w;
						uint32_t *dst = pixels + ty * phys_width + offset;
						memcpy(dst, src, tile_w * 4);
					}
					free(tile);
				}
			}

			// Draw sysinfo widget tile if enabled
			if (app_config.show_sysinfo) {
				int slot = get_sysinfo_slot_index();
				int offset = get_offset_for_icon(slot) * buffer_scale;
				int tile_w = (app_config.sysinfo_tile_width > 0 ?
									 app_config.sysinfo_tile_width :
									 app_config.icon_size) *
					buffer_scale;
				int tile_h = icon_size;
				uint32_t *tile = malloc(tile_w * tile_h * 4);
				if (tile) {
					sysinfo_draw_tile(tile, tile_w, tile_h, &app_config,
						get_corner_flags(slot));
					for (int ty = 0; ty < tile_h; ty++) {
						uint32_t *src = tile + ty * tile_w;
						uint32_t *dst = pixels + ty * phys_width + offset;
						memcpy(dst, src, tile_w * 4);
					}
					free(tile);
				}
			}
		}

		// Fill spacing gaps between adjacent bg-enabled tiles
		{
			int is_vert = (app_config.position == POSITION_LEFT ||
				app_config.position == POSITION_RIGHT);
			fill_bg_gaps(is_vert, icon_size);
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

// Forward declarations for symbols used in teardown/rebuild / layer_closed
// that are defined later in this translation unit.
static struct wl_output *find_output_by_name(const char *name);
static const struct wl_surface_listener surface_listener;
static const struct wp_fractional_scale_v1_listener fractional_scale_listener;
static const struct zwlr_layer_surface_v1_listener layer_listener;
static void rebuild_layer_surface(void);

// Set to 1 after teardown_layer_surface() so that the next output_done event
// triggers rebuild_layer_surface() once the output is ready.
static int pending_layer_rebuild = 0;

// ---------------------------------------------------------------------------
// teardown_layer_surface
//
// Destroys the layer surface, SHM buffer, and wl_surface proxies.
// Does NOT create new ones — call rebuild_layer_surface() once the output
// is back and ready (signalled by output_done).
// ---------------------------------------------------------------------------
static void
teardown_layer_surface(void)
{
	if (layer_surface) {
		zwlr_layer_surface_v1_destroy(layer_surface);
		layer_surface = NULL;
	}
	if (buffer) {
		if (pixels && pixels_size) {
			munmap(pixels, pixels_size);
			pixels_size = 0;
		}
		wl_buffer_destroy(buffer);
		buffer = NULL;
		pixels = NULL;
	}
	if (fractional_scale_obj) {
		wp_fractional_scale_v1_destroy(fractional_scale_obj);
		fractional_scale_obj = NULL;
	}
	if (viewport_obj) {
		wp_viewport_destroy(viewport_obj);
		viewport_obj = NULL;
	}
	if (surface) {
		wl_surface_destroy(surface);
		surface = NULL;
	}
	pending_layer_rebuild = 1;
	if (verbose)
		printf("[DBG] Layer surface torn down — waiting for output\n");
}

// ---------------------------------------------------------------------------
// rebuild_layer_surface
//
// Creates a fresh wl_surface + layer_surface.  Called from output_done once
// the output has re-announced itself after a hotplug / source change.
// ---------------------------------------------------------------------------
static void
rebuild_layer_surface(void)
{
	pending_layer_rebuild = 0;
	if (verbose)
		printf("[DBG] Rebuilding layer surface\n");

	surface = wl_compositor_create_surface(compositor);
	wl_surface_add_listener(surface, &surface_listener, NULL);

	if (fractional_scale_manager && viewporter) {
		fractional_scale_obj =
			wp_fractional_scale_manager_v1_get_fractional_scale(
				fractional_scale_manager, surface);
		wp_fractional_scale_v1_add_listener(fractional_scale_obj,
			&fractional_scale_listener, NULL);
		viewport_obj = wp_viewporter_get_viewport(viewporter, surface);
	}

	struct wl_output *target_output = NULL;
	if (app_config.output_name && app_config.output_name[0])
		target_output = find_output_by_name(app_config.output_name);

	int total_count = get_total_widget_count();
	int is_vertical = (app_config.position == POSITION_LEFT ||
		app_config.position == POSITION_RIGHT);
	int icon_span = 0;
	for (int i = 0; i < total_count; i++) {
		if (i > 0)
			icon_span += app_config.icon_spacing;
		icon_span += get_slot_size(i, is_vertical);
	}
	int cross_size = app_config.icon_size;

	uint32_t wl_layer = ZWLR_LAYER_SHELL_V1_LAYER_TOP;
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
	default:
		wl_layer = ZWLR_LAYER_SHELL_V1_LAYER_TOP;
		break;
	}

	layer_surface = zwlr_layer_shell_v1_get_layer_surface(layer_shell, surface,
		target_output, wl_layer, "labar");
	zwlr_layer_surface_v1_set_layer(layer_surface, wl_layer);

	uint32_t anchor;
	int bar_width, bar_height_actual;
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
	zwlr_layer_surface_v1_set_size(layer_surface, bar_width, bar_height_actual);
	zwlr_layer_surface_v1_set_exclusive_zone(layer_surface,
		app_config.exclusive_zone);
	{
		int bs = app_config.border_space;
		zwlr_layer_surface_v1_set_margin(layer_surface, bs, bs, bs, bs);
	}
	zwlr_layer_surface_v1_add_listener(layer_surface, &layer_listener, NULL);
	wl_surface_commit(surface);
}

// Called when the compositor explicitly closes the layer surface (output
// removed).  Tear down and wait for the output to come back.
static void
layer_closed(void *data, struct zwlr_layer_surface_v1 *surf)
{
	(void)data;
	(void)surf;
	if (verbose)
		printf("[DBG] Layer surface closed by compositor\n");
	// registry_remove may have already called teardown_layer_surface();
	// if so, pending_layer_rebuild is already set — nothing more to do.
	if (!pending_layer_rebuild)
		teardown_layer_surface();
}

// Seat and pointer listeners moved to seat.c

static const struct zwlr_layer_surface_v1_listener layer_listener = {
	.configure = layer_configure,
	.closed = layer_closed,
};

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// xdg-output listener — logical geometry and human-readable output name
// ---------------------------------------------------------------------------
static struct output_info *
find_output_info(struct wl_output *wl_out)
{
	for (int i = 0; i < n_outputs; i++)
		if (outputs[i].wl_out == wl_out)
			return &outputs[i];
	return NULL;
}

static struct output_info *
find_or_create_output_info(struct wl_output *wl_out)
{
	struct output_info *oi = find_output_info(wl_out);
	if (oi)
		return oi;
	if (n_outputs >= MAX_OUTPUTS)
		return NULL;
	oi = &outputs[n_outputs++];
	oi->wl_out = wl_out;
	oi->xdg_out = NULL;
	oi->name[0] = '\0';
	oi->description[0] = '\0';
	oi->logical_x = oi->logical_y = 0;
	oi->logical_w = oi->logical_h = 0;
	return oi;
}

static void
xdg_output_logical_position(void *data, struct zxdg_output_v1 *xdg_out,
	int32_t x, int32_t y)
{
	(void)xdg_out;
	struct output_info *oi = (struct output_info *)data;
	if (!oi)
		return;
	oi->logical_x = x;
	oi->logical_y = y;
}

static void
xdg_output_logical_size(void *data, struct zxdg_output_v1 *xdg_out, int32_t w,
	int32_t h)
{
	(void)xdg_out;
	struct output_info *oi = (struct output_info *)data;
	if (!oi)
		return;
	oi->logical_w = w;
	oi->logical_h = h;
}

static void
xdg_output_done(void *data, struct zxdg_output_v1 *xdg_out)
{
	(void)xdg_out;
	struct output_info *oi = (struct output_info *)data;
	if (!oi)
		return;
	if (verbose >= 1)
		printf("[DBG] xdg-output: name='%s' desc='%s' "
			   "logical=%dx%d+%d+%d\n",
			oi->name, oi->description, oi->logical_w, oi->logical_h,
			oi->logical_x, oi->logical_y);
}

static void
xdg_output_name(void *data, struct zxdg_output_v1 *xdg_out, const char *name)
{
	(void)xdg_out;
	struct output_info *oi = (struct output_info *)data;
	if (!oi)
		return;
	snprintf(oi->name, sizeof(oi->name), "%s", name);
	if (verbose >= 1)
		printf("[DBG] Output name: %s\n", oi->name);
}

static void
xdg_output_description(void *data, struct zxdg_output_v1 *xdg_out,
	const char *description)
{
	(void)xdg_out;
	struct output_info *oi = (struct output_info *)data;
	if (!oi)
		return;
	snprintf(oi->description, sizeof(oi->description), "%s", description);
	if (verbose >= 1)
		printf("[DBG] Output description: %s\n", oi->description);
}

static const struct zxdg_output_v1_listener xdg_output_listener = {
	.logical_position = xdg_output_logical_position,
	.logical_size = xdg_output_logical_size,
	.done = xdg_output_done,
	.name = xdg_output_name,
	.description = xdg_output_description,
};

// Create a zxdg_output_v1 for a wl_output and start listening.
static void
bind_xdg_output(struct wl_output *wl_out)
{
	if (!xdg_output_manager || !wl_out)
		return;
	struct output_info *oi = find_or_create_output_info(wl_out);
	if (!oi || oi->xdg_out)
		return; // already bound
	oi->xdg_out =
		zxdg_output_manager_v1_get_xdg_output(xdg_output_manager, wl_out);
	zxdg_output_v1_add_listener(oi->xdg_out, &xdg_output_listener, oi);
}

// Find a wl_output* by its xdg-output name (e.g. "eDP-1").
// Returns NULL if not found or if xdg-output is unavailable.
static struct wl_output *
find_output_by_name(const char *name)
{
	if (!name || !name[0])
		return NULL;
	for (int i = 0; i < n_outputs; i++) {
		if (strcmp(outputs[i].name, name) == 0)
			return outputs[i].wl_out;
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// wl_output listener — reads the display's buffer scale for HiDPI
// ---------------------------------------------------------------------------
static void
output_geometry(void *data, struct wl_output *wl_output, int32_t x, int32_t y,
	int32_t physical_width, int32_t physical_height, int32_t subpixel,
	const char *make, const char *model, int32_t transform)
{
	(void)data;
	(void)wl_output;
	(void)x;
	(void)y;
	(void)physical_width;
	(void)physical_height;
	(void)subpixel;
	(void)make;
	(void)model;
	(void)transform;
	// Not needed for scale detection
}

static void
output_mode(void *data, struct wl_output *wl_output, uint32_t flags,
	int32_t width, int32_t height, int32_t refresh)
{
	(void)data;
	(void)wl_output;
	(void)flags;
	(void)width;
	(void)height;
	(void)refresh;
	// Not needed for scale detection
}

static void
output_done(void *data, struct wl_output *wl_output)
{
	(void)data;
	(void)wl_output;
	// All output properties delivered; buffer_scale is already applied.
	// If a layer surface teardown is pending (output came back after removal),
	// now is the right time to rebuild.
	if (pending_layer_rebuild)
		rebuild_layer_surface();
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

	// Recompute the net tile width at the new scale.
	if (app_config.show_net) {
		int phys_w = net_compute_tile_size(&app_config);
		app_config.net_tile_width = (buffer_scale > 1) ?
			(phys_w + buffer_scale - 1) / buffer_scale :
			phys_w;
	}

	// Recompute the sysinfo tile width at the new scale.
	if (app_config.show_sysinfo) {
		int phys_w = sysinfo_compute_tile_size(&app_config);
		app_config.sysinfo_tile_width = (buffer_scale > 1) ?
			(phys_w + buffer_scale - 1) / buffer_scale :
			phys_w;
	}

	// Destroy the stale buffer; layer_configure will create a new one.
	if (buffer) {
		if (pixels && pixels_size) {
			munmap(pixels, pixels_size);
			pixels_size = 0;
		}
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
	(void)data;
	(void)wl_output;
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
	(void)data;
	(void)surf;
	if (verbose >= 2) {
		struct output_info *oi = find_output_info(entered);
		if (oi && oi->name[0])
			printf("[DBG²] Surface entered output '%s'\n", oi->name);
		else
			printf("[DBG²] Surface entered output %p\n", (void *)entered);
	}

	if (entered && entered != output) {
		output = entered;
		// Listener and xdg-output were already bound in registry_add for
		// all known outputs; no need to re-add them here.
		bind_xdg_output(output); // no-op if already bound
	}
}

static void
surface_leave(void *data, struct wl_surface *surf, struct wl_output *left)
{
	(void)data;
	(void)surf;
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
	(void)data;
	(void)obj;
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
	(void)data;
	(void)version;
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
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		// Bind all outputs (not just the first); wl_output v2 for scale events.
		// The global `output` pointer tracks the currently active one;
		// surface_enter updates it when the bar moves to a different output.
		struct wl_output *wl_out =
			wl_registry_bind(reg, name, &wl_output_interface, 2);
		struct output_info *oi = find_or_create_output_info(wl_out);
		if (oi)
			oi->registry_name = name;
		wl_output_add_listener(wl_out, &output_listener, NULL);
		bind_xdg_output(wl_out);
		if (!output)
			output = wl_out; // first output becomes the active one
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
	} else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
		xdg_output_manager =
			wl_registry_bind(reg, name, &zxdg_output_manager_v1_interface, 3);
		if (verbose >= 2)
			printf("[DBG²] Bound to zxdg_output_manager_v1\n");
		// Retroactively bind xdg-output for any wl_output already known.
		// (wl_output may be announced before zxdg_output_manager_v1.)
		if (output)
			bind_xdg_output(output);
	}
}

static void
registry_remove(void *data, struct wl_registry *reg, uint32_t name)
{
	(void)data;
	(void)reg;
	if (verbose >= 2)
		printf("[DBG²] Global object removed: name=%u\n", name);

	// Check if a tracked wl_output was removed.
	// When an output disappears (e.g. display source change), the compositor
	// removes its global.  We must release our proxy and clear the slot so
	// that a subsequent wl_registry_global for the same (or a new) output
	// is bound fresh.  We also clear the global `output` pointer if the
	// removed output was the active one — surface_enter will update it
	// once the bar re-enters an output.
	for (int i = 0; i < n_outputs; i++) {
		if (outputs[i].registry_name != name)
			continue;

		if (verbose >= 2)
			printf("[DBG²] Removing output '%s' (registry name %u)\n",
				outputs[i].name, name);

		// Release xdg-output proxy if we have one
		if (outputs[i].xdg_out) {
			zxdg_output_v1_destroy(outputs[i].xdg_out);
			outputs[i].xdg_out = NULL;
		}

		// Track whether this was the output the bar is currently on
		int was_active = (outputs[i].wl_out == output);

		// If this was the active output, clear the global pointer
		if (was_active)
			output = NULL;

		// Destroy the wl_output proxy (bound at v2; release requires v3)
		wl_output_destroy(outputs[i].wl_out);
		outputs[i].wl_out = NULL;

		// Compact the array by swapping with the last entry
		if (i < n_outputs - 1)
			outputs[i] = outputs[n_outputs - 1];
		n_outputs--;

		// If the bar's output just disappeared, tear down the layer surface.
		// The compositor will not send layer_closed reliably; we do it here.
		// rebuild_layer_surface() will be called from output_done once the
		// output re-announces itself.
		if (was_active)
			teardown_layer_surface();

		break;
	}
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
	// For running LSan's leak check phase
	signal(SIGTERM, handle_sigterm);

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
				"  ~/%s/%s\n",
				argv[0], CONFIG_DIR, CONFIG_NAME);
			return 0;
		case 'v':
			verbose++;
			break;
		case 'V':
			printf("%s\n", VERSION);
			return 0;
		case 'c': {
#ifndef HAVE_GTK4
			fprintf(stderr,
				"labar was built without GTK4; --config is not available.\n");
			return 1;
#else
			// dlopen the config plugin only now — GTK4/Mesa stay out of memory
			// during normal bar operation
			void *lib = dlopen("liblabar-config.so", RTLD_LAZY | RTLD_LOCAL);
			if (!lib) {
				// Fall back to well-known install path
				lib = dlopen(LABAR_LIB_DIR "/liblabar-config.so",
					RTLD_LAZY | RTLD_LOCAL);
			}
			if (!lib) {
				fprintf(stderr, "labar: cannot load config plugin: %s\n",
					dlerror());
				return 1;
			}
			int (*run)(void) = dlsym(lib, "config_window_run");
			if (!run) {
				fprintf(stderr, "labar: config plugin missing symbol: %s\n",
					dlerror());
				dlclose(lib);
				return 1;
			}
			int ret = run();
			dlclose(lib);
			return ret;
#endif
		}
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

	// A third roundtrip receives zxdg_output_v1 name/description/done events
	// so output names are available for the output= config key lookup below.
	if (xdg_output_manager)
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

	// Compute the net widget tile width and seed the byte counters.
	if (app_config.show_net) {
		int phys_w = net_compute_tile_size(&app_config);
		app_config.net_tile_width = (buffer_scale > 1) ?
			(phys_w + buffer_scale - 1) / buffer_scale :
			phys_w;
		net_widget_init(&app_config);
	}

	// Compute the sysinfo widget tile width and seed CPU counters.
	if (app_config.show_sysinfo) {
		int phys_w = sysinfo_compute_tile_size(&app_config);
		app_config.sysinfo_tile_width = (buffer_scale > 1) ?
			(phys_w + buffer_scale - 1) / buffer_scale :
			phys_w;
		sysinfo_widget_init();
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
	// Regular slots each occupy icon_size; text-widget slots use their
	// computed tile widths on horizontal bars, but icon_size on vertical bars.
	int total_count = get_total_widget_count();
	int is_vertical_bar = (app_config.position == POSITION_LEFT ||
		app_config.position == POSITION_RIGHT);
	int icon_span = 0;
	for (int i = 0; i < total_count; i++) {
		if (i > 0)
			icon_span += app_config.icon_spacing;
		icon_span += get_slot_size(i, is_vertical_bar);
	}
	// The bar cross-dimension (height for horizontal bar, width for vertical)
	// is always icon_size.
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
	// If output= is configured, pin the bar to that specific output;
	// otherwise pass NULL to let the compositor choose.
	struct wl_output *target_output = NULL;
	if (app_config.output_name && app_config.output_name[0]) {
		target_output = find_output_by_name(app_config.output_name);
		if (target_output) {
			if (verbose)
				printf("[DBG] Pinning bar to output '%s'\n",
					app_config.output_name);
		} else {
			fprintf(stderr,
				"warn: output '%s' not found — "
				"using compositor default\n",
				app_config.output_name);
		}
	}
	layer_surface = zwlr_layer_shell_v1_get_layer_surface(layer_shell, surface,
		target_output, wl_layer, "labar");

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

	// Push the bar away from the screen edge
	{
		int bs = app_config.border_space;
		zwlr_layer_surface_v1_set_margin(layer_surface, bs, bs, bs, bs);
	}

	zwlr_layer_surface_v1_add_listener(layer_surface, &layer_listener, NULL);

	// Initial commit triggers the configure event from the compositor
	wl_surface_commit(surface);

	// Persistent state for the date widget repaint timer.
	// Initialised to -1 so the first call to date_widget_needs_repaint()
	// always triggers an immediate repaint.
	int date_last_minute = -1;
	int date_last_second = -1;

	// Persistent state for the network and sysinfo widget repaint timers.
	// Initialised to -1 so the first sample fires immediately.
	int net_last_second = -1;
	int sysinfo_last_second = -1;

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

	if (verbose && app_config.show_net)
		printf("[DBG] Net widget enabled (iface: %s)\n",
			app_config.net_iface ? app_config.net_iface : "auto");

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

		// 1b. Flush popup surface if open
		if (calendar_popup_is_open())
			wl_display_flush(display);

		// 2. Dispatch all already-queued events without blocking
		if (wl_display_dispatch_pending(display) < 0)
			break;

		// 2b. Execute deferred calendar popup close — after dispatching
		//     all pending events so no in-flight events reference the
		//     about-to-be-destroyed surface proxies.
		calendar_popup_dispatch();

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

		// 3b. Repaint net tile once per second
		if (app_config.show_net) {
			if (net_widget_needs_repaint(&net_last_second)) {
				net_repaint_tile(surface);
				wl_display_flush(display);
			}
		}

		// 3c. Repaint sysinfo tile once per second
		if (app_config.show_sysinfo) {
			if (sysinfo_widget_needs_repaint(&sysinfo_last_second,
					app_config.sysinfo_percpu)) {
				sysinfo_repaint_tile(surface);
				wl_display_flush(display);
			}
		}

		// 4. Compute the timeout for this iteration
		int timeout_ms;
		if (!app_config.show_date && !app_config.show_net &&
			!app_config.show_sysinfo) {
			// No timer-driven widgets — block indefinitely on Wayland events
			timeout_ms = -1;
		} else if (app_config.show_net || app_config.show_sysinfo ||
			needs_seconds) {
			// Net/sysinfo widgets need 1-second resolution.
			// Date widget with seconds directive also needs this.
			// Use 200 ms so we never miss a tick due to scheduling jitter.
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
	cairo_debug_reset_static_data();
	wl_display_disconnect(display);
	return 0;
}
