#define _GNU_SOURCE // Required for memfd_create()

#include <cairo.h>
#include <fcntl.h>
#include <getopt.h>
#include <librsvg/rsvg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "config.h"
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
				if (verbose)
					printf("[DBG] Drawing icon %d at x=%d: "
						"%s (size=%dx%d)\n",
						i, x_offset,
						app_config.apps[i]->icon,
						icon_size, icon_size);

				// Create a temporary buffer for this icon tile
				uint32_t *tile_data =
					malloc(icon_size * icon_size * 4);
				memset(tile_data, 0, icon_size * icon_size * 4);

				// Draw the SVG onto the tile buffer
				draw_svg(app_config.apps[i]->icon, tile_data,
					icon_size, icon_size);

				// Blit the tile onto the main buffer at the
				// appropriate offset
				for (int ty = 0; ty < icon_size; ty++) {
					uint32_t *src =
						tile_data + (ty * icon_size);
					uint32_t *dst = pixels
						+ ((ty)*surf_width) + x_offset;
					memcpy(dst, src, icon_size * 4);
				}

				free(tile_data);
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
		{"verbose", no_argument, 0, 'v'}, {0, 0, 0, 0}};

	while ((opt = getopt_long(argc, argv, "v", long_options, NULL)) != -1) {
		switch (opt) {
		case 'v':
			verbose++;
			break;
		default:
			fprintf(stderr,
				"Usage: %s [-v|--verbose] [-v|--verbose]\n",
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
