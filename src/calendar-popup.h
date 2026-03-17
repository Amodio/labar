#ifndef CALENDAR_POPUP_H
#define CALENDAR_POPUP_H

// ---------------------------------------------------------------------------
// calendar_popup
//
// A self-contained Wayland layer-shell overlay that renders the current
// year's 12-month calendar using Cairo, positioned above the date tile.
// Clicking anywhere on the popup (or pressing any key) dismisses it.
//
// Call calendar_popup_toggle() from the date tile click handler.
// Call calendar_popup_dispatch() from the main event loop each iteration
// so the popup can process its own Wayland events.
// ---------------------------------------------------------------------------

// Toggle the calendar popup (open if closed, close if open).
void calendar_popup_toggle(void);

// Returns the popup's wl_surface, or NULL if not open.
// Used by seat.c to identify pointer events on the popup.
struct wl_surface *calendar_popup_get_surface(void);

// Process any pending popup Wayland events.
// Call once per main-loop iteration. Returns 0 when the popup is closed.
int calendar_popup_dispatch(void);

// Returns 1 if the popup is currently visible.
int calendar_popup_is_open(void);

#endif /* CALENDAR_POPUP_H */
