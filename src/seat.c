#include "seat.h"
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include "config.h"
#include "exec.h"

// Forward declarations for drawing functions (defined in main.c)
extern void draw_icon(const char *path, uint32_t *data, int width, int height);
extern void draw_text(uint32_t *data, int width, int height, const char *text,
	int y_offset, unsigned int color);

// ---------------------------------------------------------------------------
// Pointer listeners — handle mouse events
// ---------------------------------------------------------------------------
void
pointer_enter(void *data, struct wl_pointer *wl_pointer, uint32_t serial,
	struct wl_surface *surface, wl_fixed_t surface_x, wl_fixed_t surface_y)
{
	if (verbose)
		printf("[DBG] Pointer entered surface at (%.2f, %.2f)\n",
			wl_fixed_to_double(surface_x), wl_fixed_to_double(surface_y));
}

void
pointer_leave(void *data, struct wl_pointer *wl_pointer, uint32_t serial,
	struct wl_surface *surface)
{
	if (verbose)
		printf("[DBG] Pointer left surface\n");

	// Treat leaving the surface as moving to no icon: clear the hover label
	// on whichever icon was last highlighted and reset the tracking index.
	if (last_hovered_icon >= 0 && app_config.label_mode == LABEL_MODE_HOVER &&
		buffer) {
		int icon_size = app_config.icon_size;
		int idx = last_hovered_icon;

		if (idx < app_config.count && app_config.apps[idx]->icon) {
			int x_offset = idx * icon_size;

			uint32_t *tile = malloc(icon_size * icon_size * 4);
			if (tile) {
				memset(tile, 0, icon_size * icon_size * 4);
				draw_icon(app_config.apps[idx]->icon, tile, icon_size,
					icon_size);
				// No label — pointer has left the surface

				for (int ty = 0; ty < icon_size; ty++) {
					uint32_t *src = tile + ty * icon_size;
					uint32_t *dst = pixels + ty * surf_width + x_offset;
					memcpy(dst, src, icon_size * 4);
				}
				free(tile);

				wl_surface_attach(surface, buffer, 0, 0);
				wl_surface_damage(surface, 0, 0, surf_width, surf_height);
				wl_surface_commit(surface);
			}
		}
	}

	last_hovered_icon = -1;
}

void
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
			int repaint_indices[2] = {last_hovered_icon, icon_index};
			for (int r = 0; r < 2; r++) {
				int idx = repaint_indices[r];
				if (idx < 0 || idx >= app_config.count)
					continue;
				if (!app_config.apps[idx]->icon)
					continue;

				int x_offset = idx * icon_size;

				// Re-render the SVG into a fresh tile
				uint32_t *tile = malloc(icon_size * icon_size * 4);
				if (!tile)
					continue;
				memset(tile, 0, icon_size * icon_size * 4);
				draw_icon(app_config.apps[idx]->icon, tile, icon_size,
					icon_size);

				// Add label only for the newly hovered icon
				if (idx == icon_index) {
					int baseline = icon_size - app_config.label_offset;
					draw_text(tile, icon_size, icon_size,
						app_config.apps[idx]->name, baseline,
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
			wl_surface_damage(surface, 0, 0, surf_width, surf_height);
			wl_surface_commit(surface);
		}

		last_hovered_icon = icon_index;
		if (icon_index >= 0 && verbose >= 1) {
			printf("[DBG] Hovering over icon/app [%d] '%s'\n", icon_index,
				app_config.apps[icon_index]->name);
		}
	}

	// Only print detailed motion at verbose level 2 to avoid spam
	if (verbose >= 3) {
		printf("[DBG³] Pointer motion at (%.2f, %.2f)\n", current_pointer_x,
			current_pointer_y);
	}
}

void
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

	const char *state_name =
		(state == WL_POINTER_BUTTON_STATE_PRESSED) ? "PRESSED" : "RELEASED";

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
				button_name, state_name, current_pointer_x, current_pointer_y);
		}
	}
}

void
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
				direction, icon_index, app_config.apps[icon_index]->name,
				scroll_value);
		}
	} else {
		if (verbose) {
			printf("[DBG] Scroll %s at (%.2f, %.2f) (value=%.2f)\n", direction,
				current_pointer_x, current_pointer_y, scroll_value);
		}
	}
}

void
pointer_frame(void *data, struct wl_pointer *wl_pointer)
{
	// End of pointer event frame
}

void
pointer_axis_source(void *data, struct wl_pointer *wl_pointer,
	uint32_t axis_source)
{
	// Axis source event (wheel, finger, continuous, etc.)
}

void
pointer_axis_stop(void *data, struct wl_pointer *wl_pointer, uint32_t time,
	uint32_t axis)
{
	// Axis stop event
}

void
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
		printf("[DBG] Scroll %s on icon/app #%d '%s' (steps=%d)\n", direction,
			icon_index, app_config.apps[icon_index]->name, abs(discrete));
	} else {
		if (verbose) {
			printf("[DBG] Scroll %s at (%.2f, %.2f) (steps=%d)\n", direction,
				current_pointer_x, current_pointer_y, abs(discrete));
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
void
seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities)
{
	if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
		pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(pointer, &pointer_listener, NULL);
		if (verbose)
			printf("[DBG] Pointer capability detected and bound\n");
	}
}

void
seat_name(void *data, struct wl_seat *seat, const char *name)
{
	if (verbose)
		printf("[DBG] Seat name: %s\n", name);
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

const struct wl_seat_listener *
get_seat_listener(void)
{
	return &seat_listener;
}

const struct wl_pointer_listener *
get_pointer_listener(void)
{
	return &pointer_listener;
}
