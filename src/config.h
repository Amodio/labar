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

#endif // CONFIG_H
