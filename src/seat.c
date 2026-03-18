#include "seat.h"
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include "calendar-popup.h"
#include "config.h"
#include "exec.h"
#include "widget-date.h"
#include "widget-net.h"
#include "widget-sysinfo.h"
#include "widget-volume.h"

// Extern declarations for global state (defined in main.c)
extern void draw_icon(const char *path, uint32_t *data, int width, int height);
extern void draw_text(uint32_t *data, int width, int height, const char *text,
	int y_offset, unsigned int color);
extern int buffer_scale; // HiDPI scale factor (1 = normal, 2 = 2x HiDPI)
extern int phys_width;	 // Physical buffer width  (surf_width  * buffer_scale)
extern int phys_height;	 // Physical buffer height (surf_height * buffer_scale)

// Convenience: index of first app slot and helper to map slot → app index
/* get_app_first_slot() is defined in main.c and exported via seat.h */
#define APP_FIRST_SLOT() (get_app_first_slot())
#define SLOT_TO_APP(slot) ((slot) - APP_FIRST_SLOT())

// Track which wl_surface the pointer is currently over
static struct wl_surface *pointer_surface = NULL;

void
pointer_enter(void *data, struct wl_pointer *wl_pointer, uint32_t serial,
	struct wl_surface *surface, wl_fixed_t surface_x, wl_fixed_t surface_y)
{
	(void)data;
	(void)wl_pointer;
	(void)serial;
	pointer_surface = surface;
	if (verbose >= 2)
		printf("[DBG²] Pointer entered surface at (%.2f, %.2f)\n",
			wl_fixed_to_double(surface_x), wl_fixed_to_double(surface_y));
}

void
pointer_leave(void *data, struct wl_pointer *wl_pointer, uint32_t serial,
	struct wl_surface *surface)
{
	(void)data;
	(void)wl_pointer;
	(void)serial;
	(void)surface;
	pointer_surface = NULL;
	if (verbose >= 2)
		printf("[DBG²] Pointer left surface\n");

	// Treat leaving the surface as moving to no icon: clear the hover label
	// on whichever icon was last highlighted and reset the tracking index.
	if (last_hovered_icon >= 0 && app_config.label_mode == LABEL_MODE_HOVER &&
		surface && buffer) {
		int icon_size = app_config.icon_size * buffer_scale;
		int idx = last_hovered_icon;
		int offset = get_offset_for_icon(idx) * buffer_scale;
		int is_vertical = (app_config.position == POSITION_LEFT ||
			app_config.position == POSITION_RIGHT);

		uint32_t *tile = malloc(icon_size * icon_size * 4);
		if (tile) {
			memset(tile, 0, icon_size * icon_size * 4);

			int date_slot = get_date_slot_index();
			int volume_slot = get_volume_slot_index();
			int net_slot = get_net_slot_index();

			if (app_config.show_volume && idx == volume_slot) {
				// Redraw volume icon without label
				int percent = 0, muted = 0;
				volume_get_info(&percent, &muted);
				draw_icon(volume_get_icon_path(percent, muted), tile, icon_size,
					icon_size);
			} else if (net_slot >= 0 && idx == net_slot) {
				// Net tile: width may differ from icon_size
				free(tile);
				int along_px = get_slot_size(idx, is_vertical) * buffer_scale;
				int tile_w = is_vertical ? icon_size : along_px;
				int tile_h = icon_size;
				tile = malloc(tile_w * tile_h * 4);
				if (!tile)
					goto done_leave;
				net_draw_tile(tile, tile_w, tile_h, &app_config,
					get_corner_flags(idx));
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
				tile = NULL;
				if (surface && buffer) {
					wl_surface_attach(surface, buffer, 0, 0);
					wl_surface_damage(surface, 0, 0, surf_width, surf_height);
					wl_surface_commit(surface);
				}
				goto done_leave;
			} else if (date_slot >= 0 && idx == date_slot) {
				// Redraw date tile — uses tile_width × icon_size, not a square
				free(tile);
				int along_px = get_slot_size(idx, is_vertical) * buffer_scale;
				int tile_w = is_vertical ? icon_size : along_px;
				int tile_h = icon_size;
				tile = malloc(tile_w * tile_h * 4);
				if (!tile)
					goto done_leave;
				date_draw_tile(tile, tile_w, tile_h, &app_config,
					get_corner_flags(idx));
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
				tile = NULL;
				if (surface && buffer) {
					wl_surface_attach(surface, buffer, 0, 0);
					wl_surface_damage(surface, 0, 0, surf_width, surf_height);
					wl_surface_commit(surface);
				}
				goto done_leave;
			} else if (app_config.show_sysinfo &&
				idx == get_sysinfo_slot_index()) {
				// Redraw sysinfo tile on leave
				if (verbose)
					printf("[DBG] pointer leave sysinfo widget\n");
				free(tile);
				int along_px =
					get_slot_size(get_sysinfo_slot_index(), is_vertical) *
					buffer_scale;
				int tile_w = is_vertical ? icon_size : along_px;
				int tile_h = icon_size;
				tile = malloc(tile_w * tile_h * 4);
				if (!tile)
					goto done_leave;
				sysinfo_draw_tile(tile, tile_w, tile_h, &app_config,
					get_corner_flags(idx));
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
				tile = NULL;
				if (surface && buffer) {
					wl_surface_attach(surface, buffer, 0, 0);
					wl_surface_damage(surface, 0, 0, surf_width, surf_height);
					wl_surface_commit(surface);
				}
				goto done_leave;
			} else if (SLOT_TO_APP(idx) >= 0 &&
				SLOT_TO_APP(idx) < app_config.count &&
				app_config.apps[SLOT_TO_APP(idx)]->icon) {
				// Redraw app icon without label
				draw_icon(app_config.apps[SLOT_TO_APP(idx)]->icon, tile,
					icon_size, icon_size);
			} else {
				free(tile);
				goto done_leave;
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

			if (surface && buffer) {
				wl_surface_attach(surface, buffer, 0, 0);
				wl_surface_damage(surface, 0, 0, surf_width, surf_height);
				wl_surface_commit(surface);
			}
		}
	}
done_leave:

	last_hovered_icon = -1;
}

void
pointer_motion(void *data, struct wl_pointer *wl_pointer, uint32_t time,
	wl_fixed_t surface_x, wl_fixed_t surface_y)
{
	(void)data;
	(void)wl_pointer;
	(void)time;
	current_pointer_x = wl_fixed_to_double(surface_x);
	current_pointer_y = wl_fixed_to_double(surface_y);

	// Don't process bar hover while popup is open/closing
	if (calendar_popup_get_surface() != NULL)
		return;

	// Determine which icon is under the pointer based on layout orientation
	int is_vertical = (app_config.position == POSITION_LEFT ||
		app_config.position == POSITION_RIGHT);
	double coord = is_vertical ? current_pointer_y : current_pointer_x;
	int icon_index = get_icon_at_position(coord);

	// Only repaint when hovering icon changes to avoid spam
	if (icon_index != last_hovered_icon) {
		// In HOVER mode, repaint the affected icon tiles to add/remove
		// the label
		if (app_config.label_mode == LABEL_MODE_HOVER && surface && buffer) {
			int icon_size = app_config.icon_size * buffer_scale;
			int date_slot = get_date_slot_index();
			int volume_slot = get_volume_slot_index();
			int net_slot = get_net_slot_index();

			int repaint_indices[2] = {last_hovered_icon, icon_index};
			for (int r = 0; r < 2; r++) {
				int idx = repaint_indices[r];
				if (idx < 0)
					continue;

				int offset = get_offset_for_icon(idx) * buffer_scale;

				uint32_t *tile = malloc(icon_size * icon_size * 4);
				if (!tile)
					continue;
				memset(tile, 0, icon_size * icon_size * 4);

				int sysinfo_slot = get_sysinfo_slot_index();

				if (app_config.show_volume && idx == volume_slot) {
					// Volume slot
					int percent = 0, muted = 0;
					volume_get_info(&percent, &muted);
					draw_icon(volume_get_icon_path(percent, muted), tile,
						icon_size, icon_size);
					if (idx == icon_index) {
						char label[16];
						volume_get_label(label, sizeof(label), percent, muted);
						int baseline =
							icon_size - app_config.label_offset * buffer_scale;
						draw_text(tile, icon_size, icon_size, label, baseline,
							app_config.label_color);
					}
				} else if (net_slot >= 0 && idx == net_slot) {
					// Net tile: display-only (no hover label)
					free(tile);
					tile = NULL;
					int along_px =
						get_slot_size(idx, is_vertical) * buffer_scale;
					int tile_w = is_vertical ? icon_size : along_px;
					int tile_h = icon_size;
					uint32_t *ntile = malloc(tile_w * tile_h * 4);
					if (!ntile)
						continue;
					net_draw_tile(ntile, tile_w, tile_h, &app_config,
						get_corner_flags(idx));
					if (is_vertical) {
						for (int ty = 0; ty < tile_h; ty++) {
							uint32_t *src = ntile + ty * tile_w;
							uint32_t *dst = pixels + (offset + ty) * phys_width;
							memcpy(dst, src, tile_w * 4);
						}
					} else {
						for (int ty = 0; ty < tile_h; ty++) {
							uint32_t *src = ntile + ty * tile_w;
							uint32_t *dst = pixels + ty * phys_width + offset;
							memcpy(dst, src, tile_w * 4);
						}
					}
					free(ntile);
					continue;
				} else if (date_slot >= 0 && idx == date_slot) {
					// Date tile: height = icon_size, width depends on
					// orientation
					free(tile);
					tile = NULL;
					int along_px =
						get_slot_size(idx, is_vertical) * buffer_scale;
					int tile_w = is_vertical ? icon_size : along_px;
					int tile_h = icon_size;
					uint32_t *dtile = malloc(tile_w * tile_h * 4);
					if (!dtile)
						continue;
					date_draw_tile(dtile, tile_w, tile_h, &app_config,
						get_corner_flags(idx));
					if (is_vertical) {
						for (int ty = 0; ty < tile_h; ty++) {
							uint32_t *src = dtile + ty * tile_w;
							uint32_t *dst = pixels + (offset + ty) * phys_width;
							memcpy(dst, src, tile_w * 4);
						}
					} else {
						for (int ty = 0; ty < tile_h; ty++) {
							uint32_t *src = dtile + ty * tile_w;
							uint32_t *dst = pixels + ty * phys_width + offset;
							memcpy(dst, src, tile_w * 4);
						}
					}
					free(dtile);
					continue;
				} else if (sysinfo_slot >= 0 && idx == sysinfo_slot) {
					// Sysinfo tile: display-only (no hover label)
					free(tile);
					tile = NULL;
					int along_px =
						get_slot_size(idx, is_vertical) * buffer_scale;
					int tile_w = is_vertical ? icon_size : along_px;
					int tile_h = icon_size;
					uint32_t *stile = malloc(tile_w * tile_h * 4);
					if (!stile)
						continue;
					sysinfo_draw_tile(stile, tile_w, tile_h, &app_config,
						get_corner_flags(idx));
					if (is_vertical) {
						for (int ty = 0; ty < tile_h; ty++) {
							uint32_t *src = stile + ty * tile_w;
							uint32_t *dst = pixels + (offset + ty) * phys_width;
							memcpy(dst, src, tile_w * 4);
						}
					} else {
						for (int ty = 0; ty < tile_h; ty++) {
							uint32_t *src = stile + ty * tile_w;
							uint32_t *dst = pixels + ty * phys_width + offset;
							memcpy(dst, src, tile_w * 4);
						}
					}
					free(stile);
					continue;
				} else {
					// Regular app slot
					int app_idx = SLOT_TO_APP(idx);
					if (app_idx < 0 || app_idx >= app_config.count ||
						!app_config.apps[app_idx]->icon) {
						free(tile);
						continue;
					}
					draw_icon(app_config.apps[app_idx]->icon, tile, icon_size,
						icon_size);
					if (idx == icon_index) {
						int baseline =
							icon_size - app_config.label_offset * buffer_scale;
						draw_text(tile, icon_size, icon_size,
							app_config.apps[app_idx]->name, baseline,
							app_config.label_color);
					}
				}

				// Blit the tile onto the main buffer
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

			// Commit the updated surface
			wl_surface_attach(surface, buffer, 0, 0);
			wl_surface_damage(surface, 0, 0, surf_width, surf_height);
			wl_surface_commit(surface);
		}

		last_hovered_icon = icon_index;
		if (icon_index >= 0 && verbose >= 1) {
			int date_slot = get_date_slot_index();
			int volume_slot = get_volume_slot_index();
			int net_slot = get_net_slot_index();
			int sysinfo_slot = get_sysinfo_slot_index();
			if (date_slot >= 0 && icon_index == date_slot) {
				char tooltip[64];
				date_get_tooltip(tooltip, sizeof(tooltip));
				printf("[DBG] Hovering over date widget (%s)\n", tooltip);
			} else if (app_config.show_volume && icon_index == volume_slot) {
				printf("[DBG] Hovering over volume widget\n");
			} else if (net_slot >= 0 && icon_index == net_slot) {
				printf("[DBG] Hovering over net widget\n");
			} else if (sysinfo_slot >= 0 && icon_index == sysinfo_slot) {
				printf("[DBG] Hovering over sysinfo widget\n");
			} else {
				int app_idx = SLOT_TO_APP(icon_index);
				if (app_idx >= 0 && app_idx < app_config.count)
					printf("[DBG] Hovering over icon/app #%d '%s'\n", app_idx,
						app_config.apps[app_idx]->name);
			}
		}
	}

	// Only print detailed motion at verbose level 2 to avoid spam
	if (verbose >= 4) {
		printf("[DBG⁴] Pointer motion at (%.2f, %.2f)\n", current_pointer_x,
			current_pointer_y);
	}
}

// ---------------------------------------------------------------------------
// volume_repaint_tile
//
// Re-renders the volume icon slot after a state change (click or scroll).
// Queries the current volume, picks the right PNG, and optionally overlays
// the label (shown when label_mode is ALWAYS, or when the pointer is
// currently hovering over the slot in HOVER mode).
// ---------------------------------------------------------------------------
static void
volume_repaint_tile(struct wl_surface *surface)
{
	if (!buffer || !app_config.show_volume)
		return;

	int icon_size = app_config.icon_size * buffer_scale; // physical pixels
	int is_vertical = (app_config.position == POSITION_LEFT ||
		app_config.position == POSITION_RIGHT);
	int offset = get_offset_for_icon(get_volume_slot_index()) * buffer_scale;

	int percent = 0, muted = 0;
	volume_get_info(&percent, &muted);

	uint32_t *tile = malloc(icon_size * icon_size * 4);
	if (!tile)
		return;

	memset(tile, 0, icon_size * icon_size * 4);
	draw_icon(volume_get_icon_path(percent, muted), tile, icon_size, icon_size);

	// Draw label when always-on, or when the pointer is hovering over the slot
	int hovering = (last_hovered_icon == get_volume_slot_index());
	if (app_config.label_mode == LABEL_MODE_ALWAYS ||
		(app_config.label_mode == LABEL_MODE_HOVER && hovering)) {
		char label[16];
		volume_get_label(label, sizeof(label), percent, muted);
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

	wl_surface_attach(surface, buffer, 0, 0);
	wl_surface_damage(surface, 0, 0, surf_width, surf_height);
	wl_surface_commit(surface);
}

void
pointer_button(void *data, struct wl_pointer *wl_pointer, uint32_t serial,
	uint32_t time, uint32_t button, uint32_t state)
{
	(void)data;
	(void)wl_pointer;
	(void)serial;
	(void)time;
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

	// If the click is on the calendar popup surface, close it and stop
	if (state == WL_POINTER_BUTTON_STATE_PRESSED && calendar_popup_is_open() &&
		pointer_surface == calendar_popup_get_surface()) {
		if (verbose >= 1)
			printf("[CAL] click on popup — closing\n");
		calendar_popup_toggle();
		return;
	}

	// If the popup is open/closing and pointer is on its surface, ignore bar
	// events
	if (calendar_popup_get_surface() != NULL &&
		pointer_surface == calendar_popup_get_surface())
		return;

	// Calculate which icon was clicked based on current pointer position,
	// accounting for spacing and layout orientation
	int is_vertical = (app_config.position == POSITION_LEFT ||
		app_config.position == POSITION_RIGHT);
	double coord = is_vertical ? current_pointer_y : current_pointer_x;
	int icon_index = get_icon_at_position(coord);
	int date_slot = get_date_slot_index();
	int volume_slot = get_volume_slot_index();
	int net_slot = get_net_slot_index();
	int sysinfo_slot = get_sysinfo_slot_index();

	int app_idx = SLOT_TO_APP(icon_index);
	if (icon_index >= 0 && app_idx >= 0 && app_idx < app_config.count) {
		if (verbose) {
			printf("[DBG] Mouse button %s (%s) on icon/app #%d '%s'\n",
				button_name, state_name, app_idx,
				app_config.apps[app_idx]->name);
		}
		if (button == BTN_LEFT && state == WL_POINTER_BUTTON_STATE_PRESSED) {
			launch_app(app_config.apps[app_idx]);
		}
	} else if (app_config.show_volume && icon_index == volume_slot) {
		if (verbose) {
			printf("[DBG] Mouse button %s (%s) on volume widget\n", button_name,
				state_name);
		}
		if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
			volume_handle_click(button, app_config.volume_exec);
			volume_repaint_tile(surface);
		}
	} else if (net_slot >= 0 && icon_index == net_slot) {
		// Net widget is display-only; log the click but take no action
		if (verbose) {
			printf("[DBG] Mouse button %s (%s) on net widget\n", button_name,
				state_name);
		}
	} else if (sysinfo_slot >= 0 && icon_index == sysinfo_slot) {
		if (verbose) {
			printf("[DBG] Mouse button %s (%s) on sysinfo widget\n",
				button_name, state_name);
		}
		if (button == BTN_LEFT && state == WL_POINTER_BUTTON_STATE_PRESSED) {
			if (app_config.sysinfo_exec && app_config.sysinfo_exec[0])
				launch_command(app_config.sysinfo_exec);
		}
	} else if (date_slot >= 0 && icon_index == date_slot) {
		if (verbose) {
			char tooltip[64];
			date_get_tooltip(tooltip, sizeof(tooltip));
			printf("[DBG] Mouse button %s (%s) on date widget (%s)\n",
				button_name, state_name, tooltip);
		}
		if (button == BTN_LEFT && state == WL_POINTER_BUTTON_STATE_PRESSED) {
			calendar_popup_toggle();
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
	(void)data;
	(void)wl_pointer;
	(void)time;
	double scroll_value = wl_fixed_to_double(value);
	const char *direction = "";
	int icon_index = -1;

	if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
		direction = (scroll_value > 0) ? "DOWN" : "UP";
	} else if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
		direction = (scroll_value > 0) ? "RIGHT" : "LEFT";
	}

	// Determine which icon is under the pointer based on layout orientation
	int is_vertical = (app_config.position == POSITION_LEFT ||
		app_config.position == POSITION_RIGHT);
	double coord = is_vertical ? current_pointer_y : current_pointer_x;
	icon_index = get_icon_at_position(coord);
	int date_slot = get_date_slot_index();
	int volume_slot = get_volume_slot_index();
	int net_slot = get_net_slot_index();
	int sysinfo_slot = get_sysinfo_slot_index();

	int app_idx = SLOT_TO_APP(icon_index);
	if (icon_index >= 0 && app_idx >= 0 && app_idx < app_config.count) {
		if (verbose) {
			printf("[DBG] Scroll %s on icon/app #%d '%s' (value=%.2f)\n",
				direction, app_idx, app_config.apps[app_idx]->name,
				scroll_value);
		}
	} else if (app_config.show_volume && icon_index == volume_slot) {
		if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
			if (verbose)
				printf("[DBG] Scroll %s on volume widget (value=%.2f)\n",
					direction, scroll_value);
			volume_handle_scroll(scroll_value < 0);
			volume_repaint_tile(surface);
		}
	} else if (net_slot >= 0 && icon_index == net_slot) {
		if (verbose)
			printf("[DBG] Scroll %s on net widget (no action)\n", direction);
	} else if (sysinfo_slot >= 0 && icon_index == sysinfo_slot) {
		if (verbose)
			printf("[DBG] Scroll %s on sysinfo widget (no action)\n",
				direction);
	} else if (date_slot >= 0 && icon_index == date_slot) {
		if (verbose)
			printf("[DBG] Scroll %s on date widget (no action)\n", direction);
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
	(void)data;
	(void)wl_pointer;
	// End of pointer event frame
}

void
pointer_axis_source(void *data, struct wl_pointer *wl_pointer,
	uint32_t axis_source)
{
	(void)data;
	(void)wl_pointer;
	(void)axis_source;
	// Axis source event (wheel, finger, continuous, etc.)
}

void
pointer_axis_stop(void *data, struct wl_pointer *wl_pointer, uint32_t time,
	uint32_t axis)
{
	(void)data;
	(void)wl_pointer;
	(void)time;
	(void)axis;
	// Axis stop event
}

void
pointer_axis_discrete(void *data, struct wl_pointer *wl_pointer, uint32_t axis,
	int32_t discrete)
{
	(void)data;
	(void)wl_pointer;
	// Discrete axis event (for wheel/stepped scrolling)
	const char *direction = "";
	int icon_index = -1;

	if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
		direction = (discrete > 0) ? "DOWN" : "UP";
	} else if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
		direction = (discrete > 0) ? "RIGHT" : "LEFT";
	}

	// Determine which icon is under the pointer
	int is_vertical_d = (app_config.position == POSITION_LEFT ||
		app_config.position == POSITION_RIGHT);
	double coord_d = is_vertical_d ? current_pointer_y : current_pointer_x;
	icon_index = get_icon_at_position(coord_d);
	int date_slot = get_date_slot_index();
	int volume_slot = get_volume_slot_index();
	int net_slot = get_net_slot_index();
	int sysinfo_slot = get_sysinfo_slot_index();

	int app_idx = SLOT_TO_APP(icon_index);
	if (icon_index >= 0 && app_idx >= 0 && app_idx < app_config.count) {
		printf("[DBG] Scroll %s on icon/app #%d '%s' (steps=%d)\n", direction,
			app_idx, app_config.apps[app_idx]->name, abs(discrete));
	} else if (app_config.show_volume && icon_index == volume_slot) {
		if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
			if (verbose)
				printf("[DBG] Scroll %s on volume widget (steps=%d)\n",
					direction, abs(discrete));
			volume_handle_scroll(discrete < 0);
			volume_repaint_tile(surface);
		}
	} else if (net_slot >= 0 && icon_index == net_slot) {
		if (verbose)
			printf("[DBG] Scroll %s on net widget (no action)\n", direction);
	} else if (sysinfo_slot >= 0 && icon_index == sysinfo_slot) {
		if (verbose)
			printf("[DBG] Scroll %s on sysinfo widget (no action)\n",
				direction);
	} else if (date_slot >= 0 && icon_index == date_slot) {
		if (verbose)
			printf("[DBG] Scroll %s on date widget (no action)\n", direction);
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
	(void)data;
	if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
		pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(pointer, &pointer_listener, NULL);
		if (verbose >= 2)
			printf("[DBG²] Pointer capability detected and bound\n");
	}
}

void
seat_name(void *data, struct wl_seat *seat, const char *name)
{
	(void)data;
	(void)seat;
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
