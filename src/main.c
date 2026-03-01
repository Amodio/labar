#define _GNU_SOURCE // Required for memfd_create()

#include <cairo.h>
#include <fcntl.h>
#include <getopt.h>
#include <librsvg/rsvg.h>
#include <linux/input-event-codes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "config.h"
#include "exec.h"
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
	struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, width,
		height, stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool); // The buffer keeps a reference; pool can go
	close(fd);		   // The mapping keeps the data alive

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
static void
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
static void
draw_svg(const char *path, uint32_t *data, int width, int height)
{
	GError *error = NULL;
	RsvgHandle *handle = rsvg_handle_new_from_file(path, &error);
	if (!handle) {
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
	if (!rsvg_handle_get_intrinsic_size_in_pixels(handle, &svg_w, &svg_h)
		|| svg_w == 0 || svg_h == 0) {
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

static void
draw_png(const char *path, uint32_t *data, int width, int height)
{
	cairo_surface_t *img = cairo_image_surface_create_from_png(path);
	if (cairo_surface_status(img) != CAIRO_STATUS_SUCCESS) {
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
	cairo_scale(cr, (double)width / img_w,
		(double)height / img_h);

	cairo_set_source_surface(cr, img, 0, 0);
	cairo_paint(cr);

	cairo_destroy(cr);
	cairo_surface_destroy(cs);
	cairo_surface_destroy(img);
}

static void
draw_icon(const char *path, uint32_t *data, int width, int height)
{
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

		// Draw each loaded application icon horizontally
		int x_offset = 0;
		int icon_size =
			app_config.icon_size; // Use configured icon size
		for (int i = 0; i < app_config.count; i++) {
			if (x_offset + icon_size > surf_width)
				break; // Don't draw beyond buffer width

			if (app_config.apps[i]->icon) {
				if (verbose >= 2) {
					printf("[DBG²] Drawing icon %d at x=%d: %s (size=%dx%d)\n",
						i, x_offset,
						app_config.apps[i]->icon,
						icon_size, icon_size);
				}

				// Create a temporary buffer for this icon tile
				uint32_t *tile_data =
					malloc(icon_size * icon_size * 4);
				memset(tile_data, 0, icon_size * icon_size * 4);

				// Draw the SVG onto the tile buffer
				draw_icon(app_config.apps[i]->icon, tile_data,
					icon_size, icon_size);

				// Draw the app name as a text overlay on the
				// icon Create a temporary buffer for the icon
				// with text overlay
				uint32_t *text_overlay =
					malloc(icon_size * icon_size * 4);
				memcpy(text_overlay, tile_data,
					icon_size * icon_size * 4);

				free(tile_data);

				// Draw the label only when label_mode is
				// ALWAYS. HOVER mode is handled at render-time
				// via pointer events; NEVER skips drawing
				// entirely.
				if (app_config.label_mode == LABEL_MODE_ALWAYS) {
					// Baseline placed above the bottom edge
					// by label_offset pixels so ascenders
					// and descenders stay inside the tile.
					int baseline = icon_size - app_config.label_offset;
					draw_text(text_overlay, icon_size,
						icon_size,
						app_config.apps[i]->name,
						baseline,
						app_config.label_color);
				}

				// Blit the text overlay onto the main buffer
				for (int ty = 0; ty < icon_size; ty++) {
					uint32_t *src =
						text_overlay + (ty * icon_size);
					uint32_t *dst = pixels
						+ ((ty)*surf_width) + x_offset;
					memcpy(dst, src, icon_size * 4);
				}

				free(text_overlay);
				x_offset += icon_size;
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

// ---------------------------------------------------------------------------
// Pointer listeners — handle mouse events
// ---------------------------------------------------------------------------
static void
pointer_enter(void *data, struct wl_pointer *wl_pointer, uint32_t serial,
	struct wl_surface *surface, wl_fixed_t surface_x, wl_fixed_t surface_y)
{
	if (verbose)
		printf("[DBG] Pointer entered surface at (%.2f, %.2f)\n",
			wl_fixed_to_double(surface_x),
			wl_fixed_to_double(surface_y));
}

static void
pointer_leave(void *data, struct wl_pointer *wl_pointer, uint32_t serial,
	struct wl_surface *surface)
{
	if (verbose)
		printf("[DBG] Pointer left surface\n");

	// Treat leaving the surface as moving to no icon: clear the hover label
	// on whichever icon was last highlighted and reset the tracking index.
	if (last_hovered_icon >= 0 &&
		app_config.label_mode == LABEL_MODE_HOVER && buffer) {
		int icon_size = app_config.icon_size;
		int idx = last_hovered_icon;

		if (idx < app_config.count && app_config.apps[idx]->icon) {
			int x_offset = idx * icon_size;

			uint32_t *tile = malloc(icon_size * icon_size * 4);
			if (tile) {
				memset(tile, 0, icon_size * icon_size * 4);
				draw_icon(app_config.apps[idx]->icon, tile,
					icon_size, icon_size);
				// No label — pointer has left the surface

				for (int ty = 0; ty < icon_size; ty++) {
					uint32_t *src = tile + ty * icon_size;
					uint32_t *dst = pixels + ty * surf_width + x_offset;
					memcpy(dst, src, icon_size * 4);
				}
				free(tile);

				wl_surface_attach(surface, buffer, 0, 0);
				wl_surface_damage(surface, 0, 0, surf_width,
					surf_height);
				wl_surface_commit(surface);
			}
		}
	}

	last_hovered_icon = -1;
}

static void
pointer_motion(void *data, struct wl_pointer *wl_pointer, uint32_t time,
	wl_fixed_t surface_x, wl_fixed_t surface_y)
{
	current_pointer_x = wl_fixed_to_double(surface_x);
	current_pointer_y = wl_fixed_to_double(surface_y);

	// Determine which icon is under the pointer
	int icon_index = -1;
	if (app_config.icon_size > 0) {
		icon_index = (int)(current_pointer_x / app_config.icon_size);
		if (icon_index < 0 || icon_index >= app_config.count)
			icon_index = -1;
	}

	// Only print when hovering icon changes to avoid spam
	if (icon_index != last_hovered_icon) {
		// In HOVER mode, repaint the affected icon tiles to add/remove
		// the label
		if (app_config.label_mode == LABEL_MODE_HOVER && buffer) {
			int icon_size = app_config.icon_size;

			// Helper lambda-like: repaint one icon tile with or
			// without label We do this for the previously hovered
			// icon (remove label) and the newly hovered icon (add
			// label).
			int repaint_indices[2] = {
				last_hovered_icon, icon_index};
			for (int r = 0; r < 2; r++) {
				int idx = repaint_indices[r];
				if (idx < 0 || idx >= app_config.count)
					continue;
				if (!app_config.apps[idx]->icon)
					continue;

				int x_offset = idx * icon_size;

				// Re-render the SVG into a fresh tile
				uint32_t *tile =
					malloc(icon_size * icon_size * 4);
				if (!tile)
					continue;
				memset(tile, 0, icon_size * icon_size * 4);
				draw_icon(app_config.apps[idx]->icon, tile,
					icon_size, icon_size);

				// Add label only for the newly hovered icon
				if (idx == icon_index) {
					int baseline = icon_size - app_config.label_offset;
					draw_text(tile, icon_size, icon_size,
						app_config.apps[idx]->name,
						baseline,
						app_config.label_color);
				}

				// Blit the tile onto the main buffer
				for (int ty = 0; ty < icon_size; ty++) {
					uint32_t *src = tile + ty * icon_size;
					uint32_t *dst = pixels + ty * surf_width + x_offset;
					memcpy(dst, src, icon_size * 4);
				}
				free(tile);
			}

			// Commit the updated surface
			wl_surface_attach(surface, buffer, 0, 0);
			wl_surface_damage(surface, 0, 0, surf_width,
				surf_height);
			wl_surface_commit(surface);
		}

		last_hovered_icon = icon_index;
		if (icon_index >= 0 && verbose >= 1) {
			printf("[DBG] Hovering over icon/app [%d] '%s'\n",
				icon_index, app_config.apps[icon_index]->name);
		}
	}

	// Only print detailed motion at verbose level 2 to avoid spam
	if (verbose >= 3) {
		printf("[DBG³] Pointer motion at (%.2f, %.2f)\n",
			current_pointer_x, current_pointer_y);
	}
}

static void
pointer_button(void *data, struct wl_pointer *wl_pointer, uint32_t serial,
	uint32_t time, uint32_t button, uint32_t state)
{
	// Determine which button was clicked
	const char *button_name;
	switch (button) {
	case BTN_LEFT:
		button_name = "LEFT";
		break;
	case BTN_MIDDLE:
		button_name = "MIDDLE";
		break;
	case BTN_RIGHT:
		button_name = "RIGHT";
		break;
	default:
		button_name = "UNKNOWN";
		break;
	}

	const char *state_name = (state == WL_POINTER_BUTTON_STATE_PRESSED) ?
		"PRESSED" :
		"RELEASED";

	// Calculate which icon was clicked based on current pointer position
	int icon_index = -1;
	if (app_config.icon_size > 0) {
		icon_index = (int)(current_pointer_x / app_config.icon_size);
		// Clamp to valid range
		if (icon_index < 0)
			icon_index = -1;
		if (icon_index >= app_config.count)
			icon_index = -1;
	}

	if (icon_index >= 0 && icon_index < app_config.count) {
		if (verbose) {
			printf("[DBG] Mouse button %s (%s) on icon/app #%d '%s'\n",
				button_name, state_name, icon_index,
				app_config.apps[icon_index]->name);
		}
		if (button == BTN_LEFT && state == WL_POINTER_BUTTON_STATE_PRESSED) {
			launch_app(app_config.apps[icon_index]);
		}
	} else {
		if (verbose) {
			printf("[DBG] Mouse button %s (%s) at (%.2f, %.2f) - no app hit\n",
				button_name, state_name, current_pointer_x,
				current_pointer_y);
		}
	}
}

static void
pointer_axis(void *data, struct wl_pointer *wl_pointer, uint32_t time,
	uint32_t axis, wl_fixed_t value)
{
	double scroll_value = wl_fixed_to_double(value);
	const char *direction = "";
	int icon_index = -1;

	if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
		direction = (scroll_value > 0) ? "DOWN" : "UP";
	} else if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
		direction = (scroll_value > 0) ? "RIGHT" : "LEFT";
	}

	// Determine which icon is under the pointer
	if (app_config.icon_size > 0) {
		icon_index = (int)(current_pointer_x / app_config.icon_size);
		if (icon_index < 0 || icon_index >= app_config.count)
			icon_index = -1;
	}

	if (icon_index >= 0) {
		if (verbose) {
			printf("[DBG] Scroll %s on icon/app #%d '%s' (value=%.2f)\n",
				direction, icon_index,
				app_config.apps[icon_index]->name, scroll_value);
		}
	} else {
		if (verbose) {
			printf("[DBG] Scroll %s at (%.2f, %.2f) (value=%.2f)\n",
				direction, current_pointer_x, current_pointer_y,
				scroll_value);
		}
	}
}

static void
pointer_frame(void *data, struct wl_pointer *wl_pointer)
{
	// End of pointer event frame
}

static void
pointer_axis_source(void *data, struct wl_pointer *wl_pointer,
	uint32_t axis_source)
{
	// Axis source event (wheel, finger, continuous, etc.)
}

static void
pointer_axis_stop(void *data, struct wl_pointer *wl_pointer, uint32_t time,
	uint32_t axis)
{
	// Axis stop event
}

static void
pointer_axis_discrete(void *data, struct wl_pointer *wl_pointer, uint32_t axis,
	int32_t discrete)
{
	// Discrete axis event (for wheel/stepped scrolling)
	const char *direction = "";
	int icon_index = -1;

	if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
		direction = (discrete > 0) ? "DOWN" : "UP";
	} else if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
		direction = (discrete > 0) ? "RIGHT" : "LEFT";
	}

	// Determine which icon is under the pointer
	if (app_config.icon_size > 0) {
		icon_index = (int)(current_pointer_x / app_config.icon_size);
		if (icon_index < 0 || icon_index >= app_config.count)
			icon_index = -1;
	}

	if (icon_index >= 0) {
		printf("[DBG] Scroll %s on icon/app #%d '%s' (steps=%d)\n",
			direction, icon_index,
			app_config.apps[icon_index]->name, abs(discrete));
	} else {
		if (verbose) {
			printf("[DBG] Scroll %s at (%.2f, %.2f) (steps=%d)\n",
				direction, current_pointer_x, current_pointer_y,
				abs(discrete));
		}
	}
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
	.frame = pointer_frame,
	.axis_source = pointer_axis_source,
	.axis_stop = pointer_axis_stop,
	.axis_discrete = pointer_axis_discrete,
};

// ---------------------------------------------------------------------------
// Seat listener — used to get the pointer
// ---------------------------------------------------------------------------
static void
seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities)
{
	if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
		pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(pointer, &pointer_listener, NULL);
		if (verbose)
			printf("[DBG] Pointer capability detected and bound\n");
	}
}

static void
seat_name(void *data, struct wl_seat *seat, const char *name)
{
	if (verbose)
		printf("[DBG] Seat name: %s\n", name);
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

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
		compositor = wl_registry_bind(reg, name,
			&wl_compositor_interface, 4);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		layer_shell = wl_registry_bind(reg, name,
			&zwlr_layer_shell_v1_interface, 1);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
		wl_seat_add_listener(seat, &seat_listener, NULL);
		if (verbose)
			printf("[DBG] Bound to wl_seat\n");
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_add,
	.global_remove = NULL, // Not handled — globals are considered permanent
};

int
main(int argc, char *argv[])
{
	// Parse command-line arguments
	int opt;
	static struct option long_options[] = {
		{"verbose", no_argument, 0, 'v'},
		{"version", no_argument, 0, 'V'},
		{0, 0, 0, 0}};

	while ((opt = getopt_long(argc, argv, "vV", long_options, NULL)) != -1) {
		switch (opt) {
		case 'v':
			verbose++;
			break;
		case 'V':
			printf("%s\n", VERSION);
			return 0;
		default:
			fprintf(stderr,
				"Usage: %s [-v|--verbose] [-V|--version]\n",
				argv[0]);
			return 1;
		}
	}

	// Load config (will create it if it doesn't exist)
	app_config = load_config();
	if (!app_config.apps || app_config.count == 0) {
		fprintf(stderr, "Failed to load configuration\n");
		return 1;
	}

	if (verbose) {
		printf("[DBG] Loaded configuration with %d application(s):\n",
			app_config.count);
		for (int i = 0; i < app_config.count; i++) {
			printf("[DBG]   [%d] %s\n", i,
				app_config.apps[i]->name);
			if (app_config.apps[i]->icon)
				printf("[DBG]       icon: %s\n",
					app_config.apps[i]->icon);
			if (app_config.apps[i]->terminal)
				printf("[DBG]       terminal: true\n");
			printf("[DBG]       exec: %s\n",
				app_config.apps[i]->exec);
		}
	}

	// Connect to the Wayland compositor
	display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "Failed to connect to Wayland display\n");
		free_config(&app_config);
		return 1;
	}

	if (verbose)
		printf("[DBG] Connected to Wayland display\n");

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

	// Calculate bar dimensions based on number of icons
	int required_width = app_config.count * app_config.icon_size;
	int bar_height = app_config.icon_size;

	if (verbose) {
		printf("[DBG] Bar dimensions: %dx%d (%d icons x %dpx each)\n",
			required_width, bar_height, app_config.count,
			app_config.icon_size);
	}

	// Promote the surface to a layer-shell surface on the TOP layer.
	// The namespace "labar" identifies our client to the compositor.
	layer_surface = zwlr_layer_shell_v1_get_layer_surface(layer_shell,
		surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "labar");

	// Anchor the bar to the output
	zwlr_layer_surface_v1_set_anchor(layer_surface,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM);

	// Request the calculated bar dimensions
	zwlr_layer_surface_v1_set_size(layer_surface, required_width,
		bar_height);

	// Reserve space so other surfaces don't overlap the dock
	zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, bar_height);

	zwlr_layer_surface_v1_add_listener(layer_surface, &layer_listener,
		NULL);

	// Initial commit triggers the configure event from the compositor
	wl_surface_commit(surface);

	// Dispatch Wayland events until the connection is lost or we exit
	while (wl_display_dispatch(display) != -1) {
	}

	free_config(&app_config);
	wl_display_disconnect(display);
	return 0;
}
