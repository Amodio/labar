#ifndef SEAT_H
#define SEAT_H

#include <wayland-client.h>
#include "config.h"

// Seat and pointer related functions
void seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities);
void seat_name(void *data, struct wl_seat *seat, const char *name);

// Pointer listener functions
void pointer_enter(void *data, struct wl_pointer *wl_pointer, uint32_t serial,
	struct wl_surface *surface, wl_fixed_t surface_x, wl_fixed_t surface_y);
void pointer_leave(void *data, struct wl_pointer *wl_pointer, uint32_t serial,
	struct wl_surface *surface);
void pointer_motion(void *data, struct wl_pointer *wl_pointer, uint32_t time,
	wl_fixed_t surface_x, wl_fixed_t surface_y);
void pointer_button(void *data, struct wl_pointer *wl_pointer, uint32_t serial,
	uint32_t time, uint32_t button, uint32_t state);
void pointer_axis(void *data, struct wl_pointer *wl_pointer, uint32_t time,
	uint32_t axis, wl_fixed_t value);
void pointer_frame(void *data, struct wl_pointer *wl_pointer);
void pointer_axis_source(void *data, struct wl_pointer *wl_pointer,
	uint32_t axis_source);
void pointer_axis_stop(void *data, struct wl_pointer *wl_pointer, uint32_t time,
	uint32_t axis);
void pointer_axis_discrete(void *data, struct wl_pointer *wl_pointer,
	uint32_t axis, int32_t discrete);

// Extern declarations for global state (defined in main.c)
extern struct wl_pointer *pointer;
extern struct wl_surface *surface;
extern struct wl_buffer *buffer;
extern uint32_t *pixels;
extern int surf_width;
extern int surf_height;
extern double current_pointer_x;
extern double current_pointer_y;
extern int last_hovered_icon;
extern int verbose;
extern Config app_config;

// Listener accessor functions
const struct wl_seat_listener *get_seat_listener(void);
const struct wl_pointer_listener *get_pointer_listener(void);

// Slot index helpers (defined in main.c, used by seat.c)
int get_date_slot_index(void);
int get_volume_slot_index(void);
int get_net_slot_index(void);
int get_app_first_slot(void);
int get_offset_for_icon(int icon_index);
int get_icon_at_position(double coord);

// Drawing functions (defined in main.c, used by seat.c)
void draw_icon(const char *path, uint32_t *data, int width, int height);
void draw_text(uint32_t *data, int width, int height, const char *text,
	int y_offset, unsigned int color);

#endif // SEAT_H
