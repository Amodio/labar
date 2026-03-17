#ifndef CONFIG_H
#define CONFIG_H

// Global verbose flag for debug output
extern int verbose;

// Opaque struct representing a parsed .desktop file entry
typedef struct {
	char *name;
	char *exec;
	char *icon;
	int terminal;
} DesktopEntry;

// Controls when the application name label is drawn over its icon
typedef enum {
	LABEL_MODE_ALWAYS, // Always draw the label
	LABEL_MODE_HOVER,  // Draw only when the pointer is over the icon
	LABEL_MODE_NEVER,  // Never draw the label
} LabelMode;

// Bar position on screen
typedef enum {
	POSITION_BOTTOM, // Bottom of screen (horizontal orientation)
	POSITION_TOP,	 // Top of screen (horizontal orientation)
	POSITION_LEFT,	 // Left side of screen (vertical orientation)
	POSITION_RIGHT,	 // Right side of screen (vertical orientation)
} Position;

// Layer for the layer-shell surface
typedef enum {
	LAYER_BACKGROUND, // Beneath everything (for wallpapers, lock screens)
	LAYER_BOTTOM,	  // Below normal windows (default for docks/panels)
	LAYER_TOP,		  // Above normal windows
	LAYER_OVERLAY,	  // On top of everything (for notifications, overlays)
} Layer;

// Configuration structure holding loaded applications
typedef struct {
	DesktopEntry **apps;
	int count;
	int icon_size; // Default icon size in pixels (default: 64)
	// When to display the app-name label (default: hover)
	LabelMode label_mode;
	unsigned int label_color; // Label color as 0xAARRGGBB (default: 0xFFFFFFFF)
	int label_size;			  // Font size in points for the label (default: 10)
	// Vertical distance in pixels from the bottom edge of the icon tile to
	// the text baseline.
	//
	// Precise boundaries:
	//   0           – baseline sits exactly on the bottom edge of the tile;
	//                 descenders (g, p, y …) will be fully clipped.
	//   ~font_size  – baseline is one full em above the bottom; the text
	//                 floats near the middle of the icon for a 10 px font
	//                 and a typical icon_size.
	//   icon_size   – baseline at the top edge; entire glyph is above the
	//                 tile and invisible.
	//
	// Recommended range: 4–16 px. Default is 10 (a small raise from the
	// bottom that keeps ascenders and most descenders inside the tile).
	int label_offset;
	int exclusive_zone; // Exclusive zone for zwlr_layer_surface_v1 (default: 0)
	int icon_spacing;	// Spacing between icons in pixels (default: 0)
	Position position;	// Bar position on screen (default: bottom)
	Layer layer;		// Layer-shell layer (default: bottom)

	// ---------------------------------------------------------------------------
	// Volume widget
	//
	// When show_volume is non-zero an extra icon slot is appended to the bar
	// that displays the current ALSA PCM volume level.  The slot uses the PNG
	// images from /usr/share/pixmaps/labar/ and overlays the volume percentage
	// as a text label (respecting label_mode / label_color / label_size).
	//
	// Mouse bindings for the volume slot:
	//   Left-click   : amixer -q sset PCM toggle   (mute / unmute)
	//   Right-click  : foot -e alsamixer            (open full mixer)
	//   Scroll-up    : amixer -q sset PCM 4%+       (raise by 4 %)
	//   Scroll-down  : amixer -q sset PCM 4%-       (lower by 4 %)
	// ---------------------------------------------------------------------------
	int show_volume; // 1 = append volume icon, 0 = disabled (default: 0)

	// ---------------------------------------------------------------------------
	// Date / time widget
	//
	// When show_date is non-zero an extra text-only slot is appended to the
	// bar (after the volume slot if show_volume is also enabled).  The slot
	// renders two lines of text directly with Cairo — no PNG icon is required:
	//
	//   Line 1 (upper half) – date string  (date_date_format, default "%a %d")
	//   Line 2 (lower half) – time string  (date_time_format, default "%H:%M")
	//
	// Config keys in [global]:
	//   show-date          true / false                    (default: false)
	//   widget-date-format   strftime format for line 1      (default: "%a %d")
	//   widget-date-color    #RRGGBB[AA] for line 1          (default: #FFFFFF)
	//   widget-date-size     font size in pt for line 1      (default: 10)
	//   widget-date-time-format   strftime format for line 2      (default:
	//   "%H:%M") widget-date-time-color    #RRGGBB[AA] for line 2 (default:
	//   #FFFFFF) widget-date-time-size     font size in pt for line 2 (default:
	//   14) widget-date-bg-color      #RRGGBB[AA] tile background     (default:
	//   0 = transparent)
	//
	// The slot is display-only — no mouse bindings are registered.
	// The tile is redrawn automatically once per minute.
	// ---------------------------------------------------------------------------
	int show_date;				  // 1 = append date/time slot, 0 = disabled
	char *date_date_format;		  // strftime format for the date line (line 1)
	unsigned int date_date_color; // ARGB color for the date line
	int date_date_size;			  // font size in pt for the date line
	char *date_time_format;		  // strftime format for the time line (line 2)
	unsigned int date_time_color; // ARGB color for the time line
	int date_time_size;			  // font size in pt for the time line
	unsigned int date_bg_color;	  // ARGB background color for the tile
								  // 0 = fully transparent (default)
	int date_tile_width; // computed tile width in pixels along the bar axis
						 // (set by date_compute_tile_size() after load).
						 // Height is always icon_size — the bar thickness
						 // never changes.

	// ---------------------------------------------------------------------------
	// Network activity widget
	//
	// When show_net is non-zero an extra text-only slot is appended to the bar
	// (after the date slot if show_date is also enabled).  The slot renders two
	// lines of text with Cairo:
	//
	//   Line 1 (upper half) – receive  speed  (e.g. "↓ 1.2 MB/s")
	//   Line 2 (lower half) – transmit speed  (e.g. "↑  456 KB/s")
	//
	// Config keys in [global]:
	//   show-net               true / false              (default: false)
	//   widget-net-iface       interface name            (default: auto-detect)
	//   widget-net-rx-color    #RRGGBB[AA]               (default: #4FC3F7)
	//   widget-net-tx-color    #RRGGBB[AA]               (default: #EF9A9A)
	//   widget-net-size        font size in pt           (default: 9)
	//   widget-net-bg-color    #RRGGBB[AA] tile bg       (default: transparent)
	//
	// The slot is display-only — no mouse bindings are registered.
	// The tile is redrawn automatically once per second.
	// ---------------------------------------------------------------------------
	int show_net;			   // 1 = append network slot, 0 = disabled
	char *net_iface;		   // interface name (NULL = auto-detect)
	unsigned int net_rx_color; // ARGB color for the RX speed line
	unsigned int net_tx_color; // ARGB color for the TX speed line
	int net_font_size;		   // font size in pt (0 = use default)
	unsigned int net_bg_color; // ARGB tile background (0 = transparent)
	int net_tile_width;		   // computed tile width (set after load)

	// ---------------------------------------------------------------------------
	// CPU / RAM usage widget (sysinfo)
	// ---------------------------------------------------------------------------
	int show_sysinfo;				// 1 = show widget, 0 = disabled
	unsigned int sysinfo_cpu_color; // ARGB color for the CPU line
	unsigned int sysinfo_ram_color; // ARGB color for the RAM line
	int sysinfo_font_size;			// font size in pt (0 = use default)
	unsigned int sysinfo_bg_color;	// ARGB tile background (0 = transparent)
	int sysinfo_tile_width;			// computed tile width (set after load)

	// ---------------------------------------------------------------------------
	// Widget ordering
	//
	// widget_order[0..4] stores the five slot IDs in their on-bar order.
	// IDs: 0 = net, 1 = volume, 2 = date, 3 = apps, 4 = sysinfo.
	// Default order: net(0), sysinfo(4), apps(3), volume(1), date(2).
	// ---------------------------------------------------------------------------
	int widget_order[5]; // bar order of widgets + apps block
} Config;

// List all valid applications from /usr/share/applications/*.desktop
// Returns a dynamically allocated array of DesktopEntry pointers.
// The caller must free all entries using free_applications().
//
// Parameters:
//   count_out – receives the number of entries in the returned array
//
// Returns NULL if an error occurs; check count_out for the count.
DesktopEntry **list_all_applications(int *count_out);

// Free all application entries allocated by list_all_applications()
//
// Parameters:
//   entries – the array returned by list_all_applications()
//   count   – the count returned by list_all_applications()
void free_applications(DesktopEntry **entries, int count);

// Free a Config struct
void free_config(Config *cfg);

// Check if the ~/.config/labar.cfg file exists
//
// Returns:
//   1 if the file exists
//   0 if the file does not exist or cannot be accessed
int config_file_exists(void);

// Load the config file or create it if it doesn't exist
// If the file doesn't exist, calls init_config() to create it
// Returns a Config struct with loaded or created applications
//
// Returns:
//   Config struct with apps array and count
//   apps will be NULL and count 0 on failure
Config load_config(void);

// Write default config file with firefox and foot if they exist in the app list
// Writes to ~/.config/labar.cfg
// Creates ~/.config directory if it doesn't exist
//
// Parameters:
//   entries – the array returned by list_all_applications()
//   count   – the count returned by list_all_applications()
//
// Returns:
//   1 if a config file was written
//   0 if neither firefox nor foot were found, or on error
int write_default_config(DesktopEntry **entries, int count);

// Create config file if loading failed
// If config doesn't exist, tries to create it with firefox and foot
// If firefox and foot aren't found, lists all available applications
//
// Returns:
//   0 if successful (config was created)
//   1 if unable to proceed (no apps found or couldn't create config)
int init_config(void);

// ---------------------------------------------------------------------------
// Tile background corner-rounding flags (used by all widget draw_tile fns)
// ---------------------------------------------------------------------------
#define TILE_ROUND_LEFT 0x1
#define TILE_ROUND_RIGHT 0x2
#define TILE_ROUND_ALL (TILE_ROUND_LEFT | TILE_ROUND_RIGHT)

#endif // CONFIG_H
