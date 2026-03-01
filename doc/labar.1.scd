labar(1)

# NAME

labar - A Wayland layer-shell application bar with configurable icons

# SYNOPSIS

*labar* [options]

# DESCRIPTION

*labar* is a minimal Wayland layer-shell bar for displaying application icons.
It reads a configuration file to determine which applications to display and
renders their icons horizontally on the screen. The bar can be customized via
an INI-style configuration file located at *~/.config/labar.cfg*.

# OPTIONS

*-v, --verbose*
	Enable verbose debug output. Specify multiple times for increased verbosity:
	- *-v*: Normal debug messages (rendering, bar setup)
	- *-vv*: Detailed parsing information (.desktop files, config parsing,
	icon discovery)

*-V, --version*
	Print the version number and exit.

*-h, --help*
	Display help message and exit.

# CONFIGURATION

The configuration file is located at *~/.config/labar.cfg*.
If it does not exist, *labar* will attempt to auto-generate it.

## Configuration Format

The configuration file uses an INI-style format with two sections:

*[global]*
	Global settings for the bar.

	*icon_size=SIZE*
		Set the icon size in pixels. Default: 64.

*[apps]*
	Application entries. Each application requires three fields:

	*name=NAME*
		Display name of the application.

	*icon=PATH*
		Full filesystem path to the icon file (SVG or PNG).

	*exec=COMMAND*
		Command to execute when the icon is clicked.

## Example Configuration

```
# labar configuration
[global]
icon_size=64

[apps]
name=Firefox
icon=/usr/share/icons/hicolor/scalable/apps/firefox.svg
exec=/usr/lib/firefox/firefox %u

name=Foot
icon=/usr/share/icons/hicolor/scalable/apps/foot.svg
exec=foot
```

# ICON DISCOVERY

When creating a default configuration, *labar* automatically discovers the
best-quality icons for applications:

. Searches /usr/share/icons recursively
. Prioritizes SVG icons (scalable, highest quality)
. Falls back to PNG icons, sorted by size (largest first)
. Stores the full path in the configuration file

Icons can be specified manually in the configuration by providing the full
filesystem path.

# ENVIRONMENT

*HOME*
	Used to locate the configuration file at *$HOME/.config/labar.cfg*.

# FILES

*~/.config/labar.cfg*
	User configuration file. Created automatically on first run if Firefox
	or Foot are found on the system.

# EXAMPLES

Run with normal verbosity:
```
labar -v
```

Run with detailed parsing information:
```
labar -vv
```

Print the version:
```
labar -V
```

# SEE ALSO

*sway*(1), *weston*(1)

# AUTHORS

Jacques Boscq <jacques@boscq.fr>

Source code available at https://github.com/Amodio/labar
