#define _GNU_SOURCE

#include "config.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define APPS_DIR "/usr/share/applications"
#define CONFIG_DIR ".config"
#define CONFIG_NAME "labar.cfg"

// ---------------------------------------------------------------------------
// .desktop file parser
//
// Parses key-value pairs from a .desktop file section.
// .desktop files are INI-style with [Desktop Entry] as the main section.
// ---------------------------------------------------------------------------

// Trim whitespace from both ends of a string (in-place)
static char *
trim_string(char *str)
{
	if (!str)
		return str;

	// Trim leading whitespace
	while (*str && isspace((unsigned char)*str))
		str++;

	// Trim trailing whitespace
	char *end = str + strlen(str) - 1;
	while (end >= str && isspace((unsigned char)*end))
		*end-- = '\0';

	return str;
}

// Parse a single .desktop file
// Returns a DesktopEntry on success, NULL on failure
static DesktopEntry *
parse_desktop_file(const char *filepath)
{
	if (verbose >= 3)
		printf("[I/O] FOPEN (read): %s\n", filepath);

	FILE *fp = fopen(filepath, "r");
	if (!fp) {
		if (verbose >= 3)
			printf("[I/O] FOPEN FAILED: %s\n", filepath);
		return NULL;
	}

	if (verbose >= 2)
		printf("[DBG²] Parsing .desktop file: %s\n", filepath);

	DesktopEntry *entry = calloc(1, sizeof(DesktopEntry));
	if (!entry) {
		fclose(fp);
		return NULL;
	}

	char line[512];
	int no_display = 0;
	int in_desktop_section = 0;

	while (fgets(line, sizeof(line), fp)) {
		// Remove trailing newline
		line[strcspn(line, "\n")] = '\0';

		// Skip empty lines and comments
		if (line[0] == '\0' || line[0] == ';' || line[0] == '#')
			continue;

		// Check for [Desktop Entry] section
		if (strcmp(line, "[Desktop Entry]") == 0) {
			in_desktop_section = 1;
			continue;
		}

		// Stop parsing if we hit a different section
		if (line[0] == '[') {
			in_desktop_section = 0;
			continue;
		}

		if (!in_desktop_section)
			continue;

		// Parse key=value pairs
		char *eq = strchr(line, '=');
		if (!eq)
			continue;

		*eq = '\0';
		char *key = trim_string(line);
		char *value = trim_string(eq + 1);

		if (strcasecmp(key, "Name") == 0) {
			free(entry->name);
			entry->name = strdup(value);
			if (verbose >= 2)
				printf("[DBG²]   Name: %s\n", value);
		} else if (strcasecmp(key, "Exec") == 0) {
			free(entry->exec);
			entry->exec = strdup(value);
			if (verbose >= 2)
				printf("[DBG²]   Exec: %s\n", value);
		} else if (strcasecmp(key, "Icon") == 0) {
			free(entry->icon);
			entry->icon = strdup(value);
			if (verbose >= 2)
				printf("[DBG²]   Icon: %s\n", value);
		} else if (strcasecmp(key, "NoDisplay") == 0) {
			no_display = (strcasecmp(value, "true") == 0);
			if (verbose >= 2)
				printf("[DBG²]   NoDisplay: %s\n", value);
		} else if (strcasecmp(key, "Terminal") == 0) {
			entry->terminal = (strcasecmp(value, "true") == 0);
			if (verbose >= 2) {
				printf("[DBG²]   terminal: %s\n",
					entry->terminal ? "true" : "false");
			}
		}
	}

	fclose(fp);

	if (verbose >= 3)
		printf("[I/O] FCLOSE: %s\n", filepath);

	// Valid entries need at least a Name and Exec
	if (!entry->name || !entry->exec || no_display) {
		if (entry->name)
			free(entry->name);
		if (entry->exec)
			free(entry->exec);
		if (entry->icon)
			free(entry->icon);
		free(entry);
		return NULL;
	}

	return entry;
}

static void
free_desktop_entry(DesktopEntry *entry)
{
	if (!entry)
		return;
	free(entry->name);
	free(entry->exec);
	free(entry->icon);
	free(entry);
}

// Find an application by name in the entries array
static DesktopEntry *
find_app_by_name(DesktopEntry **entries, int count, const char *name)
{
	for (int i = 0; i < count; i++) {
		if (strcasecmp(entries[i]->name, name) == 0)
			return entries[i];
	}
	return NULL;
}

// Icon surface cache entry
typedef struct {
	char *icon_path;
	int size;		  // Size in pixels
	uint32_t *pixels; // Decoded pixel data
} IconSurfaceCache;

// Icon candidate for sorting and selection
typedef struct {
	char *path;
	int size; // SVG: 999 (scalable), PNG: pixel dimension
} IconCandidate;

// Comparison function for sorting icons (best to worst)
// SVG (size=999) ranks highest, then PNG sorted by size descending
static int
compare_icons(const void *a, const void *b)
{
	IconCandidate *ic_a = (IconCandidate *)a;
	IconCandidate *ic_b = (IconCandidate *)b;

	// Larger size is better (SVG at 999 comes first)
	return ic_b->size - ic_a->size;
}

// Build a heap-allocated icon path; caller must free.
// Returns NULL on allocation failure.
static char *
make_icon_path(const char *dir, const char *size, const char *name,
	const char *ext)
{
	char *p = NULL;
	asprintf(&p, "%s/%s/apps/%s.%s", dir, size, name, ext);
	return p;
}

// Find all icon variants for an application
// Returns the best quality icon path, listing all candidates if verbose
// Caller must free the returned string
static char *
find_best_icon(const char *icon_name)
{
	if (!icon_name || strlen(icon_name) == 0)
		return NULL;

	if (verbose >= 2)
		printf("[DBG²] Finding best icon for '%s'\n", icon_name);

	IconCandidate *candidates = NULL;
	int candidate_count = 0;
	int capacity = 16;

	candidates = malloc(capacity * sizeof(IconCandidate));
	if (!candidates)
		return NULL;

	const char *icon_base = "/usr/share/icons";
	DIR *base_dir = opendir(icon_base);
	if (!base_dir) {
		free(candidates);
		return NULL;
	}

	struct dirent *theme_entry;
	while ((theme_entry = readdir(base_dir))) {
		// Skip . and ..
		if (theme_entry->d_name[0] == '.')
			continue;

		char *theme_path = NULL;
		if (asprintf(&theme_path, "%s/%s", icon_base, theme_entry->d_name) < 0)
			continue;

		// Check if it's a directory
		DIR *theme_dir = opendir(theme_path);
		if (!theme_dir) {
			free(theme_path);
			continue;
		}

		struct dirent *size_entry;
		while ((size_entry = readdir(theme_dir))) {
			if (size_entry->d_name[0] == '.')
				continue;

			// Try SVG
			char *svg_path = make_icon_path(theme_path, size_entry->d_name,
				icon_name, "svg");
			if (svg_path) {
				if (access(svg_path, F_OK) == 0) {
					if (verbose >= 2)
						printf("[DBG²]   Found SVG: "
							   "%s\n",
							svg_path);
					if (candidate_count >= capacity) {
						capacity *= 2;
						IconCandidate *tmp = realloc(candidates,
							capacity * sizeof(IconCandidate));
						if (tmp)
							candidates = tmp;
						else {
							free(svg_path);
							goto cleanup;
						}
					}
					candidates[candidate_count].path = svg_path;
					candidates[candidate_count].size =
						999; // SVG is scalable (highest
							 // priority)
					candidate_count++;
				} else {
					if (verbose >= 3)
						printf("[I/O] ACCESS CHECK (stat): %s - NOT FOUND\n",
							svg_path);
					free(svg_path);
				}
			} // Try PNG
			char *png_path = make_icon_path(theme_path, size_entry->d_name,
				icon_name, "png");
			if (png_path) {
				if (access(png_path, F_OK) == 0) {
					if (verbose >= 3)
						printf("[I/O] ACCESS CHECK (stat): %s - EXISTS\n",
							png_path);
					// Extract size from directory name
					// (e.g., "256x256" -> 256)
					int size = 0;
					sscanf(size_entry->d_name, "%dx%d", &size, &size);

					if (verbose >= 2)
						printf("[DBG²]   Found PNG "
							   "(%dpx): %s\n",
							size, png_path);
					if (candidate_count >= capacity) {
						capacity *= 2;
						IconCandidate *tmp = realloc(candidates,
							capacity * sizeof(IconCandidate));
						if (tmp)
							candidates = tmp;
						else {
							free(png_path);
							goto cleanup;
						}
					}
					candidates[candidate_count].path = png_path;
					candidates[candidate_count].size = size;
					candidate_count++;
				} else {
					if (verbose >= 3)
						printf("[I/O] ACCESS CHECK (stat): %s - NOT FOUND\n",
							png_path);
					free(png_path);
				}
			}
		}

		closedir(theme_dir);
		free(theme_path);
	}

	if (verbose >= 3)
		printf("[I/O] CLOSEDIR: /usr/share/icons (base theme dir)\n");
	closedir(base_dir);

	if (candidate_count == 0) {
		if (verbose >= 2)
			printf("[DBG²] No icon candidates found for '%s'\n", icon_name);
		free(candidates);
		return NULL;
	}

	if (verbose >= 2)
		printf("[DBG²] Found %d candidate icon(s) for '%s'\n", candidate_count,
			icon_name);

	// Sort candidates from best to worst quality
	qsort(candidates, candidate_count, sizeof(IconCandidate), compare_icons);

	// List all candidates if verbose
	if (verbose >= 2) {
		printf("[DBG²] Available icons for '%s' (best to worst):\n", icon_name);
		for (int i = 0; i < candidate_count; i++) {
			const char *type = (candidates[i].size == 999) ? "SVG" : "PNG";
			printf("[DBG²]   [%d] %s (%s, %dpx)\n", i, candidates[i].path, type,
				candidates[i].size);
		}
	}

	// Return the best one
	char *best_path = strdup(candidates[0].path);

	if (verbose >= 2)
		printf("[DBG²] Best icon selected: %s\n", best_path);

cleanup:
	for (int i = 0; i < candidate_count; i++)
		free(candidates[i].path);
	free(candidates);

	return best_path;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// List all applications from APPS_DIR/*.desktop
// Allocates an array of DesktopEntry pointers; caller must free all entries
// and the array itself.
DesktopEntry **
list_all_applications(int *count_out)
{
	const char *app_dir = APPS_DIR;
	if (verbose >= 3)
		printf("[I/O] OPENDIR: %s\n", app_dir);
	DIR *dir = opendir(app_dir);
	if (!dir) {
		if (verbose >= 3)
			printf("[I/O] OPENDIR FAILED: %s\n", app_dir);
		perror("opendir");
		*count_out = 0;
		return NULL;
	}

	// First pass: count valid .desktop files
	int capacity = 16;
	int count = 0;
	DesktopEntry **entries = malloc(capacity * sizeof(DesktopEntry *));
	if (!entries) {
		closedir(dir);
		*count_out = 0;
		return NULL;
	}

	struct dirent *entry;
	while ((entry = readdir(dir))) {
		// Only process .desktop files
		if (strlen(entry->d_name) < 9)
			continue;
		if (strcmp(entry->d_name + strlen(entry->d_name) - 8, ".desktop") != 0)
			continue;

		// Build full path
		char filepath[512];
		snprintf(filepath, sizeof(filepath), "%s/%s", app_dir, entry->d_name);

		// Parse the .desktop file
		DesktopEntry *parsed = parse_desktop_file(filepath);
		if (!parsed)
			continue;

		// Grow array if needed
		if (count >= capacity) {
			capacity *= 2;
			DesktopEntry **tmp =
				realloc(entries, capacity * sizeof(DesktopEntry *));
			if (!tmp) {
				free_desktop_entry(parsed);
				goto cleanup;
			}
			entries = tmp;
		}

		entries[count++] = parsed;
	}

	if (verbose >= 3)
		printf("[I/O] CLOSEDIR: %s\n", app_dir);
	closedir(dir);
	*count_out = count;
	return entries;

cleanup:
	for (int i = 0; i < count; i++)
		free_desktop_entry(entries[i]);
	free(entries);
	closedir(dir);
	*count_out = 0;
	return NULL;
}

void
free_applications(DesktopEntry **entries, int count)
{
	if (!entries)
		return;
	for (int i = 0; i < count; i++)
		free_desktop_entry(entries[i]);
	free(entries);
}

// Free a Config struct
void
free_config(Config *cfg)
{
	if (!cfg)
		return;
	free_applications(cfg->apps, cfg->count);
	cfg->apps = NULL;
	cfg->count = 0;
}

// Check if the config file exists
int
config_file_exists(void)
{
	const char *home = getenv("HOME");
	if (!home)
		return 0;

	char config_path[512];
	snprintf(config_path, sizeof(config_path), "%s/" CONFIG_DIR "/" CONFIG_NAME,
		home);

	if (verbose >= 3)
		printf("[I/O] ACCESS CHECK (stat): %s\n", config_path);
	return access(config_path, F_OK) == 0;
}

// Parse the labar.cfg config file and load applications
// Returns a Config struct with loaded applications
static Config
parse_config_file(FILE *fp)
{
	Config cfg = {0};
	cfg.apps = malloc(10 * sizeof(DesktopEntry *));
	cfg.icon_size = 64;				   // Default icon size
	cfg.label_mode = LABEL_MODE_HOVER; // Show label on hover by default
	cfg.label_color = 0xFFFFFFFF;	   // Opaque white by default
	cfg.label_size = 10;			   // 10 pt font by default
	cfg.label_offset = 10;			   // Baseline 10 px above the bottom edge
	cfg.exclusive_zone = 0;			   // No exclusive zone by default
	cfg.icon_spacing = 0;			   // No spacing between icons by default
	cfg.position = POSITION_BOTTOM;	   // Bar at the bottom by default
	cfg.layer = LAYER_TOP;			   // Layer-shell top layer by default
	if (!cfg.apps)
		return cfg;

	int capacity = 10;
	char line[512];
	DesktopEntry *current_entry = NULL;
	int in_apps_section = 0;
	int in_global_section = 0;

	if (verbose >= 2)
		printf("[DBG²] Parsing config file\n");

	while (fgets(line, sizeof(line), fp)) {
		// Remove trailing newline
		line[strcspn(line, "\n")] = '\0';

		// Skip empty lines and comments
		if (line[0] == '\0' || line[0] == '#')
			continue;

		if (verbose >= 2)
			printf("[DBG²] Config line: %s\n", line);

		// Check for section header
		if (line[0] == '[') {
			char *end = strchr(line, ']');
			if (end) {
				// Save previous entry if any
				if (current_entry) {
					// Only add if all required fields are
					// present
					if (current_entry->name && current_entry->exec &&
						current_entry->icon) {
						if (cfg.count >= capacity) {
							capacity *= 2;
							DesktopEntry **tmp = realloc(cfg.apps,
								capacity * sizeof(DesktopEntry *));
							if (!tmp) {
								free_desktop_entry(current_entry);
								return cfg;
							}
							cfg.apps = tmp;
						}
						cfg.apps[cfg.count++] = current_entry;
						if (verbose >= 2)
							printf("[DBG²] Added "
								   "app: %s\n",
								current_entry->name);
						if (verbose >= 2)
							printf("[DBG²]   name: %s, icon: %s,%s exec: %s\n",
								current_entry->name, current_entry->icon,
								current_entry->terminal ? " terminal: true," :
														  "",
								current_entry->exec);
					} else {
						if (verbose)
							printf("[DBG] Skipping app with missing fields\n");
						if (verbose) {
							const char *n = current_entry->name ?
								current_entry->name :
								"(missing)";
							const char *ic = current_entry->icon ?
								current_entry->icon :
								"(missing)";
							const char *ex = current_entry->exec ?
								current_entry->exec :
								"(missing)";
							printf("[DBG]   name: %s, icon: %s, exec: %s\n", n,
								ic, ex);
						}
						free_desktop_entry(current_entry);
					}
					current_entry = NULL;
				} // Check which section we're entering
				if (strncmp(line, "[global]", 8) == 0) {
					in_global_section = 1;
					in_apps_section = 0;
					if (verbose >= 2)
						printf("[DBG²] Entering "
							   "[global] section\n");
					if (verbose >= 2)
						printf("[DBG²] Processing "
							   "global "
							   "configuration\n");
				} else if (strncmp(line, "[apps]", 6) == 0) {
					in_global_section = 0;
					in_apps_section = 1;
					if (verbose >= 2)
						printf("[DBG²] Entering [apps] "
							   "section\n");
					if (verbose >= 2)
						printf("[DBG²] Processing "
							   "applications\n");
				} else {
					in_global_section = 0;
					in_apps_section = 0;
					if (verbose >= 2)
						printf("[DBG²] Unknown "
							   "section: %s\n",
							line);
				}
			}
			continue;
		}

		// Parse key=value pairs
		char *eq = strchr(line, '=');
		if (!eq)
			continue;

		*eq = '\0';
		char *key = trim_string(line);
		char *value = trim_string(eq + 1);

		if (verbose >= 2)
			printf("[DBG²]   key='%s', value='%s'\n", key, value);

		// Handle global section
		if (in_global_section) {
			if (strcmp(key, "icon-size") == 0) {
				cfg.icon_size = atoi(value);
				if (verbose >= 2)
					printf("[DBG²]   icon-size: %d\n", cfg.icon_size);
			} else if (strcmp(key, "label-mode") == 0) {
				if (strcmp(value, "hover") == 0)
					cfg.label_mode = LABEL_MODE_HOVER;
				else if (strcmp(value, "never") == 0)
					cfg.label_mode = LABEL_MODE_NEVER;
				else
					cfg.label_mode = LABEL_MODE_ALWAYS;
				if (verbose >= 2)
					printf("[DBG²]   label-mode: %s\n", value);
			} else if (strcmp(key, "label-color") == 0) {
				// Accept #RRGGBB or #RRGGBBAA
				const char *hex = value;
				if (hex[0] == '#')
					hex++;
				unsigned long parsed = strtoul(hex, NULL, 16);
				if (strlen(hex) <= 6)
					cfg.label_color = 0xFF000000 | (unsigned int)parsed;
				else
					cfg.label_color = (unsigned int)parsed;
				if (verbose >= 2)
					printf("[DBG²]   label-color: 0x%08X\n", cfg.label_color);
			} else if (strcmp(key, "label-offset") == 0) {
				cfg.label_offset = atoi(value);
				if (verbose >= 2)
					printf("[DBG²]   label-offset: %d\n", cfg.label_offset);
			} else if (strcmp(key, "label-size") == 0) {
				cfg.label_size = atoi(value);
				if (verbose >= 2)
					printf("[DBG²]   label-size: %d\n", cfg.label_size);
			} else if (strcmp(key, "exclusive-zone") == 0) {
				cfg.exclusive_zone = atoi(value);
				if (verbose >= 2)
					printf("[DBG²]   exclusive-zone: %d\n", cfg.exclusive_zone);
			} else if (strcmp(key, "icon-spacing") == 0) {
				cfg.icon_spacing = atoi(value);
				if (verbose >= 2)
					printf("[DBG²]   icon-spacing: %d\n", cfg.icon_spacing);
			} else if (strcmp(key, "position") == 0) {
				if (strcmp(value, "top") == 0)
					cfg.position = POSITION_TOP;
				else if (strcmp(value, "left") == 0)
					cfg.position = POSITION_LEFT;
				else if (strcmp(value, "right") == 0)
					cfg.position = POSITION_RIGHT;
				else
					cfg.position = POSITION_BOTTOM; // Default
				if (verbose >= 2)
					printf("[DBG²]   position: %s\n", value);
			} else if (strcmp(key, "layer") == 0) {
				if (strcmp(value, "background") == 0)
					cfg.layer = LAYER_BACKGROUND;
				else if (strcmp(value, "bottom") == 0)
					cfg.layer = LAYER_BOTTOM;
				else if (strcmp(value, "overlay") == 0)
					cfg.layer = LAYER_OVERLAY;
				else
					cfg.layer = LAYER_TOP; // Default
				if (verbose >= 2)
					printf("[DBG²]   layer: %s\n", value);
			}
			continue;
		}

		// Handle apps section
		if (!in_apps_section)
			continue;

		// Check for app entry start (name field indicates a new app)
		if (strcmp(key, "name") == 0) {
			// Save previous app entry if complete
			if (current_entry) {
				if (current_entry->name && current_entry->exec &&
					current_entry->icon) {
					if (cfg.count >= capacity) {
						capacity *= 2;
						DesktopEntry **tmp = realloc(cfg.apps,
							capacity * sizeof(DesktopEntry *));
						if (!tmp) {
							free_desktop_entry(current_entry);
							return cfg;
						}
						cfg.apps = tmp;
					}
					cfg.apps[cfg.count++] = current_entry;
					if (verbose >= 2)
						printf("[DBG²] Added app: %s\n", current_entry->name);
				} else {
					if (verbose >= 2)
						printf("[DBG²] Skipping app "
							   "with missing fields\n");
					free_desktop_entry(current_entry);
				}
			}
			current_entry = calloc(1, sizeof(DesktopEntry));
			if (!current_entry)
				return cfg;
		}

		if (!current_entry)
			continue;

		if (strcmp(key, "name") == 0) {
			free(current_entry->name);
			current_entry->name = strdup(value);
			if (verbose >= 2)
				printf("[DBG²]   name: %s\n", value);
		} else if (strcmp(key, "terminal") == 0) {
			current_entry->terminal = (strcasecmp(value, "true") == 0);
			if (verbose >= 2)
				printf("[DBG²]   terminal: %s\n",
					current_entry->terminal ? "true" : "false");
		} else if (strcmp(key, "exec") == 0) {
			free(current_entry->exec);
			current_entry->exec = strdup(value);
			if (verbose >= 2)
				printf("[DBG²]   exec: %s\n", value);
		} else if (strcmp(key, "icon") == 0) {
			free(current_entry->icon);
			current_entry->icon = strdup(value);
			if (verbose >= 2)
				printf("[DBG²]   icon: %s (size: will be set "
					   "during render)\n",
					value);
		}
	}

	// Don't forget the last entry
	if (current_entry) {
		if (current_entry->name && current_entry->exec && current_entry->icon) {
			if (cfg.count >= capacity) {
				capacity *= 2;
				DesktopEntry **tmp =
					realloc(cfg.apps, capacity * sizeof(DesktopEntry *));
				if (!tmp) {
					free_desktop_entry(current_entry);
					return cfg;
				}
				cfg.apps = tmp;
			}
			cfg.apps[cfg.count++] = current_entry;
			if (verbose >= 2)
				printf("[DBG²] Added app: %s\n", current_entry->name);
		} else {
			if (verbose >= 2)
				printf("[DBG²] Skipping app with missing "
					   "fields\n");
			free_desktop_entry(current_entry);
		}
	}

	if (verbose >= 2)
		printf("[DBG²] Config file parsed: %d application(s) loaded\n",
			cfg.count);

	if (verbose >= 2) {
		printf("[DBG²] Parsing complete - icon_size: %d\n", cfg.icon_size);
		for (int i = 0; i < cfg.count; i++) {
			printf("[DBG²]   App[%d]: %s -> %s\n", i, cfg.apps[i]->name,
				cfg.apps[i]->exec);
		}
	}

	return cfg;
}

// Load the config file or create it if it doesn't exist
// If the file doesn't exist, calls init_config() to create it
// Returns a Config struct with loaded or created applications
Config
load_config(void)
{
	Config cfg = {0};

	// Try to open existing config file
	const char *home = getenv("HOME");
	if (!home)
		return cfg;

	char config_path[512];
	snprintf(config_path, sizeof(config_path), "%s/" CONFIG_DIR "/" CONFIG_NAME,
		home);

	if (verbose >= 3)
		printf("[I/O] FOPEN (read): %s\n", config_path);
	FILE *fp = fopen(config_path, "r");
	if (!fp) {
		if (verbose >= 3)
			printf("[I/O] FOPEN FAILED (will create): %s\n", config_path);
		// File doesn't exist, create it
		if (init_config() == 0) {
			// Successfully created, reload
			if (verbose >= 3)
				printf("[I/O] FOPEN (read, after create): %s\n", config_path);
			fp = fopen(config_path, "r");
			if (!fp)
				return cfg;
		} else {
			// Failed to create config
			return cfg;
		}
	}

	// Parse the config file
	cfg = parse_config_file(fp);
	if (verbose >= 3)
		printf("[I/O] FCLOSE: %s\n", config_path);
	fclose(fp);
	return cfg;
}

// Write default config to ~/.config/labar.cfg
int
write_default_config(DesktopEntry **entries, int count)
{
	const char *home = getenv("HOME");
	if (!home)
		return 0;

	// List of default applications to include
	const char *default_apps[] = {
		"keepassxc",
		"thunderbird",
		"firefox",
		"htop",
		"foot",
	};
	int default_count = sizeof(default_apps) / sizeof(default_apps[0]);

	// Create ~/.config directory if it doesn't exist
	char config_dir[512];
	snprintf(config_dir, sizeof(config_dir), "%s/" CONFIG_DIR, home);
	if (verbose >= 3)
		printf("[I/O] MKDIR: %s\n", config_dir);
	mkdir(config_dir, 0755);

	// Write config file
	char config_path[512];
	snprintf(config_path, sizeof(config_path), "%s/" CONFIG_DIR "/" CONFIG_NAME,
		home);

	if (verbose >= 3)
		printf("[I/O] FOPEN (write): %s\n", config_path);
	FILE *fp = fopen(config_path, "w");
	if (!fp) {
		perror("fopen");
		return 0;
	}

	fprintf(fp, "# labar default configuration\n");
	fprintf(fp,
		"# Edit this file to add or remove applications from the "
		"bar\n\n");

	fprintf(fp, "[global]\n");
	fprintf(fp, "icon-size=64\n");
	fprintf(fp, "# label-mode: always | hover | never\n");
	fprintf(fp, "label-mode=hover\n");
	fprintf(fp, "# label-color: hex color #RRGGBB or #RRGGBBAA\n");
	fprintf(fp, "label-color=#FFFFFF\n");
	fprintf(fp,
		"# label_offset: pixels from the bottom edge of the icon "
		"to the text baseline\n");
	fprintf(fp,
		"# 0 = bottom edge (descenders clipped), icon-size = top "
		"edge (text invisible)\n");
	fprintf(fp, "# recommended range: 4-16\n");
	fprintf(fp, "label-offset=10\n");
	fprintf(fp, "# label-size: font size in points for the app-name label\n");
	fprintf(fp, "label-size=10\n");
	fprintf(fp, "\n");
	fprintf(fp, "# exclusive-zone: interaction with other surfaces\n");
	fprintf(fp, "#   0  (default): surface will be moved to avoid occluding\n");
	fprintf(fp, "#                 surfaces with positive exclusive zone\n");
	fprintf(fp, "#   >0: surface reserves space (e.g., panel=10 prevents\n");
	fprintf(fp, "#       maximized windows from overlapping)\n");
	fprintf(fp, "#  -1: surface stretches to edges, ignoring other surfaces\n");
	fprintf(fp, "#      (e.g., wallpaper, lock screen)\n");
	fprintf(fp, "exclusive-zone=0\n");
	fprintf(fp, "\n");
	fprintf(fp, "# icon-spacing: spacing between icons in pixels\n");
	fprintf(fp, "icon-spacing=0\n");
	fprintf(fp, "\n");
	fprintf(fp, "# position: where to place the bar on screen\n");
	fprintf(fp, "#   bottom (default): horizontal bar at the bottom\n");
	fprintf(fp, "#   top:              horizontal bar at the top\n");
	fprintf(fp, "#   left:             vertical bar on the left\n");
	fprintf(fp, "#   right:            vertical bar on the right\n");
	fprintf(fp, "position=bottom\n");
	fprintf(fp, "\n");
	fprintf(fp, "# layer: layer-shell layer for the surface\n");
	fprintf(fp,
		"#   background:               beneath everything (for wallpapers)\n");
	fprintf(fp, "#   bottom:           below normal windows (for docks)\n");
	fprintf(fp, "#   top (default):    above normal windows\n");
	fprintf(fp, "#   overlay:          on top of everything\n");
	fprintf(fp, "layer=top\n\n");

	fprintf(fp, "[apps]\n");

	int written = 0;

	for (int i = 0; i < default_count; i++) {
		DesktopEntry *app = find_app_by_name(entries, count, default_apps[i]);

		if (!app)
			continue;

		char *icon_path = find_best_icon(default_apps[i]);
		if (!icon_path)
			continue;

		fprintf(fp, "name=%s\n", app->name);
		fprintf(fp, "icon=%s\n", icon_path);
		if (app->terminal)
			fprintf(fp, "terminal=true\n");
		fprintf(fp, "exec=%s\n\n", app->exec);

		free(icon_path);
		written++;
	}

	if (verbose >= 3)
		printf("[I/O] FCLOSE: %s\n", config_path);
	fclose(fp);

	// Return success only if at least one app was written
	return written > 0;
}

// Initialize the config file on first run
// If config doesn't exist, tries to create it
// Returns 0 if successful, 1 if unable to proceed
int
init_config(void)
{
	// Load all applications
	int app_count = 0;
	DesktopEntry **apps = list_all_applications(&app_count);

	if (app_count == 0) {
		fprintf(stderr, "No applications found in " APPS_DIR "\n");
		return 1;
	}

	// Try to write default config
	if (write_default_config(apps, app_count)) {
		printf("Created default config at ~/" CONFIG_DIR "/" CONFIG_NAME "\n");
		free_applications(apps, app_count);
		return 0;
	}

	// If we get here, firefox and foot weren't found - list available apps
	printf("No config file found at ~/" CONFIG_DIR "/" CONFIG_NAME "\n");
	printf("Available applications (%d found):\n\n", app_count);

	for (int i = 0; i < app_count; i++) {
		printf("[%d] %s\n", i + 1, apps[i]->name);
		if (apps[i]->icon)
			printf("    Icon: %s\n", apps[i]->icon);
		printf("    Exec: %s\n\n", apps[i]->exec);
	}

	free_applications(apps, app_count);

	fprintf(stderr,
		"\nPlease create a config file at ~/" CONFIG_DIR "/" CONFIG_NAME "\n");
	return 1;
}
