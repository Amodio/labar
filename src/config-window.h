#ifndef CONFIG_WINDOW_H
#define CONFIG_WINDOW_H

#include "config.h"

// ---------------------------------------------------------------------------
// Config window  (--config / -c)
//
// Opens a GTK window similar to wbar-config that lets the user inspect and
// edit the current labar configuration interactively.
//
// Sections shown:
//   [Global]   – icon-size, icon-spacing, label-mode, label-color,
//                label-size, label-offset, position, layer, exclusive-zone
//   [Widgets]  – show-volume, show-date, and all date/time widget settings
//   [Apps]     – ordered list of dock entries (name / icon / exec / terminal)
//                with Move Up / Move Down / Remove buttons
//
// A "Save" button writes the updated config back to ~/.config/labar.cfg and
// a "Close" button discards changes and exits the window (labar then exits
// because --config is a one-shot mode).
// ---------------------------------------------------------------------------

// Run the config window.  Loads the config, opens the GTK window, and blocks
// until the window is closed.  Returns 0 on success, 1 on error.
int config_window_run(void);

#endif /* CONFIG_WINDOW_H */
