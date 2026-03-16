#ifndef WIDGET_NET_H
#define WIDGET_NET_H

#include <stdint.h>
#include "config.h"

// ---------------------------------------------------------------------------
// Network activity widget
//
// Renders a text-only tile (no PNG icon required) that shows the current
// transmit and receive speeds for a configured network interface, sampled
// from /proc/net/dev.  Two lines are drawn directly onto the shared-memory
// pixel buffer with Cairo:
//
//   Line 1 (upper half) – receive speed   (e.g. "↓ 1.2 MB/s")
//   Line 2 (lower half) – transmit speed  (e.g. "↑  456 KB/s")
//
// Config keys in [global]:
//   show-net              true / false                (default: false)
//   widget-net-iface      interface name              (default: first active)
//   widget-net-rx-color   #RRGGBB[AA] for RX line    (default: #4FC3F7)
//   widget-net-tx-color   #RRGGBB[AA] for TX line    (default: #EF9A9A)
//   widget-net-size       font size in pt             (default: 9)
//   widget-net-bg-color   #RRGGBB[AA] tile background (default: transparent)
//
// The tile is display-only — no mouse bindings are registered.
// net_widget_needs_repaint() fires once per second so the speeds stay fresh.
// ---------------------------------------------------------------------------

// Default visual properties
#define WIDGET_NET_RX_COLOR 0xFFFF3FFA // magenta (down speed)
#define WIDGET_NET_TX_COLOR 0xFF3AFFFD // cyan    (up speed)
#define WIDGET_NET_FONT_SIZE 14		   // pt

// ---------------------------------------------------------------------------
// net_widget_init
//
// Must be called once after the config is loaded.  Seeds the internal
// counters so the first repaint shows 0 B/s instead of a garbage spike.
// Selects the interface: uses cfg->net_iface if set, otherwise picks the
// first non-loopback interface that has non-zero traffic in /proc/net/dev.
// ---------------------------------------------------------------------------
void net_widget_init(Config *cfg);

// ---------------------------------------------------------------------------
// net_compute_tile_size
//
// Measure the two text lines with Cairo and return the width (pixels) the
// network slot needs along the bar axis.  Call once after the config is
// loaded and store the result in cfg->net_tile_width.
// ---------------------------------------------------------------------------
int net_compute_tile_size(const Config *cfg);

// ---------------------------------------------------------------------------
// net_draw_tile
//
// Render the network-speed widget into a caller-supplied ARGB8888 buffer
// (width × height × 4 bytes).  Clears to transparent before drawing.
// ---------------------------------------------------------------------------
void net_draw_tile(uint32_t *data, int width, int height, const Config *cfg);

// ---------------------------------------------------------------------------
// net_widget_needs_repaint
//
// Samples /proc/net/dev, computes instantaneous RX/TX speeds, and stores
// them in the internal state.  Returns 1 when the displayed strings have
// changed (always true after a ≥ 1-second interval), 0 otherwise.
// Pass *last_second = -1 on the first call to force an immediate repaint.
// ---------------------------------------------------------------------------
int net_widget_needs_repaint(int *last_second);

#endif /* WIDGET_NET_H */
