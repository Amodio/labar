labar(1)

# NAME

labar - A Wayland layer-shell application bar with icons and a date/time widget

# SYNOPSIS

*labar* [options]

# DESCRIPTION

*labar* is a minimal Wayland layer-shell bar that displays application icons
and an optional date/time widget. It renders on any edge of the screen
(top, bottom, left, or right) using the *zwlr-layer-shell-v1* protocol.
Icons are rendered from SVG or PNG files; the bar reacts to mouse clicks,
right-clicks, and scroll events. HiDPI displays are supported via
*wl_output.scale* (integer) and *wp_fractional_scale_v1* (fractional).

The configuration file is located at *~/.config/labar.cfg*. If it does not
exist, *labar* generates a default one on first run.

# OPTIONS

*-h, --help*
	Display the help message and exit.

*-v, --verbose*
	Increase verbosity. May be specified up to four times:

	*-v*
		Level 1 — general lifecycle messages: configuration loaded,
		bar dimensions, output scale detected, HiDPI scale events,
		volume changes, and minute/second boundary repaints.

	*-vv*
		Level 2 — Wayland protocol details: globals bound
		(wl_seat, wl_output, wp_fractional_scale_manager_v1,
		wp_viewporter), surface enter/leave events, config key
		parsing, icon cache hits, pointer enter/leave/click/scroll
		events per slot.

	*-vvv*
		Level 3 — file I/O: every SVG and PNG open attempt (success
		and failure), icon cache store/lookup/miss/clear operations
		with path and size.

	*-vvvv*
		Level 4 — high-frequency events: raw pointer motion
		coordinates on every mouse move. Use only for input
		debugging; output is very noisy.

*-V, --version*
	Print the version string and exit.

# CONFIGURATION

*~/.config/labar.cfg* uses an INI-style format with a *[global]* section
and one or more *[apps]* sections.

## [global] keys

*exclusive-zone=*_N_
	Exclusive zone in pixels to reserve at the bar edge, preventing
	other surfaces from overlapping. Set to *0* to disable.
	Default: *64*.

*icon-size=*_N_
	Icon slot size in pixels (logical). Applies to both width and height
	of each app slot, and to the cross-axis thickness of the bar.
	Default: *64*.

*icon-spacing=*_N_
	Gap in pixels between icon slots. Default: *0*.

*label-mode=*always|hover|never
	When to show the text label overlay on icons.
	*always* draws labels permanently; *hover* shows them only while
	the pointer is over the slot; *never* disables them entirely.
	Default: *hover*.

*label-color=*_#RRGGBB_|_#RRGGBBAA_
	Label text color. Six-digit form is fully opaque; eight-digit form
	includes an alpha byte. Default: *#FFFFFF* (white, fully opaque).

*label-offset=*_N_
	Distance in pixels from the bottom edge of the slot to the text
	baseline. Default: *10*.

*label-size=*_N_
	Label font size in points. Default: *10*.

*position=*top|bottom|left|right
	Which screen edge to attach the bar to. Default: *bottom*.

*layer=*background|bottom|top|overlay
	Wayland layer-shell layer. Default: *top* (below normal windows).

*show-volume=*true|false
	Append a volume icon slot at the end of the bar. The slot shows the
	current ALSA PCM level; left-click toggles mute, right-click opens
	*alsamixer* in a terminal, and scroll adjusts the volume.
	Default: *true*.

*show-date=*true|false
	Append a date/time text slot at the end of the bar (after the volume
	slot if enabled). The slot renders two lines of text using Cairo;
	no icon file is required. Default: *true*.

*widget-date-format=*_FORMAT_
	*strftime*(3) format string for the upper line of the date slot.
	Default: *%a %d %B* (abbreviated weekday and day-of-month).

*widget-date-color=*_#RRGGBB_|_#RRGGBBAA_
	Color of the upper date line. Default: *#68FF3A*.

*widget-date-size=*_N_
	Font size in points for the upper date line. Default: *16*.

*widget-date-time-format=*_FORMAT_
	*strftime*(3) format string for the lower line of the date slot.
	If the format includes *%S*, *%T*, *%X*, *%c*, or *%r*, the slot
	refreshes every second; otherwise it refreshes at each minute
	boundary. Default: *%H:%M*.

*widget-date-time-color=*_#RRGGBB_|_#RRGGBBAA_
	Color of the lower time line. Default: *#FF0000*.

*widget-date-time-size=*_N_
	Font size in points for the lower time line. Default: *36*.

*widget-date-bg-color=*_#RRGGBBAA_
	Background fill color for the date/time tile. Supports alpha;
	*#00000000* is fully transparent. Default: *#00000094* (black,
	42% transparent).

## [apps] keys

Each application is defined by a consecutive block of keys inside an
*[apps]* section. Multiple applications may appear in the same section.

*name=*_NAME_
	Display name, shown as the hover label.

*icon=*_PATH_
	Full filesystem path to an SVG or PNG icon file.

*exec=*_COMMAND_
	Command executed on left-click. Parsed by *execvp*(3); *%u* and
	similar desktop-file placeholders are passed through verbatim.

*terminal=*true|false
	If *true*, the command is launched inside the terminal emulator
	defined by the *DEFAULT_TERMINAL* compile-time constant (default:
	*foot*). Default: *false*.

## Default configuration

```
# labar default configuration
# Edit this file to add or remove applications from the bar

[global]
# exclusive-zone: interaction with other surfaces
#    0: surface will be moved to avoid occluding
#       surfaces with positive exclusive zone
#   >0: surface reserves space (e.g., panel=10 prevents
#       maximized windows from overlapping)
#   -1: surface stretches to edges, ignoring other
#       surfaces (e.g., wallpaper, lock screen)
exclusive-zone=64
icon-size=64
# icon-spacing: spacing between icons in pixels
icon-spacing=0
# label-mode: always | hover | never
label-mode=hover
# label-color: hex color #RRGGBB or #RRGGBBAA
label-color=#FFFFFF
# label_offset: pixels from the bottom edge of the icon to the text baseline
#   0 = bottom edge (descenders clipped), icon-size = top edge (text invisible)
#   recommended range: 4-16
label-offset=10
# label-size: font size in points for the app-name label
label-size=10
# position: where to place the bar on screen
#   bottom (default): horizontal bar at the bottom
#   top:              horizontal bar at the top
#   left:             vertical bar on the left
#   right:            vertical bar on the right
position=bottom
# layer: layer-shell layer for the surface
#   background:       beneath everything (for wallpapers)
#   bottom:           below normal windows (for docks)
#   top (default):    above normal windows
#   overlay:          on top of everything
layer=top
# show-volume: append a volume icon at the end of the bar
#   true:  show the volume widget (default)
#   false: no volume widget
show-volume=true
# show-date: append a date/time text slot at the end of the bar
#   true:  show the date/time widget
#   false: no date/time widget (default)
show-date=true
# date/time widget — line 1 (date) style
#   widget-date-format: strftime(3) format, e.g. "%a %d %B"
widget-date-format=%a %d %B
widget-date-color=#68FF3A
widget-date-size=16
# date/time widget — line 2 (time) style
#   widget-date-time-format: strftime(3) format, e.g. "%H:%M"
widget-date-time-format=%H:%M
widget-date-time-color=#FF0000
widget-date-time-size=36
# widget-date-bg-color: background color for the date/time tile
#   format: #RRGGBBAA (alpha in last byte)
#   e.g. #00000094 = black 42% transparent (default)
#        #00000000 = fully transparent
#        #000000FF = fully opaque black
widget-date-bg-color=#00000094

[apps]
name=KeePassXC
icon=/usr/share/icons/hicolor/scalable/apps/keepassxc.svg
exec=keepassxc %f

name=Thunderbird
icon=/usr/share/icons/hicolor/256x256/apps/thunderbird.png
exec=thunderbird %u

name=Firefox
icon=/usr/share/icons/hicolor/scalable/apps/firefox.svg
exec=/usr/lib/firefox/firefox %u

name=Foot
icon=/usr/share/icons/hicolor/scalable/apps/foot.svg
exec=foot
```

# ICON DISCOVERY

When generating the default configuration, *labar* searches for icons
automatically:

. Searches */usr/share/icons* recursively for each known application.
. Prioritises SVG icons (resolution-independent).
. Falls back to PNG icons, selecting the largest available size.
. Writes the absolute path into the generated configuration file.

Icons can also be specified manually by providing a full path in *icon=*.

# HIDPI

*labar* detects the display scale automatically:

- Integer scaling is read from *wl_output.scale* events (e.g. 2× for a
  standard HiDPI display).
- Fractional scaling is read from the *wp_fractional_scale_v1* protocol
  when the compositor advertises it (e.g. 1.5× or 1.25×). A
  *wp_viewport* corrects the logical-to-physical mapping.

The SHM buffer is allocated at the physical pixel resolution and
*wl_surface_set_buffer_scale* (or *wp_viewport_set_destination* for
fractional scaling) is set accordingly. The icon cache is invalidated
and the bar is redrawn whenever the scale changes, including when the bar
moves to a different monitor via *wl_surface.enter*.

# MOUSE BINDINGS

*Application icons*
	Left-click launches the configured *exec* command.
	Right-click has no default binding.
	Scroll has no default binding.

*Volume slot* (when *show-volume=true*)
	Left-click toggles ALSA PCM mute.
	Right-click opens *alsamixer* in a terminal.
	Scroll up/down adjusts PCM volume by 4%.

*Date/time slot* (when *show-date=true*)
	Display-only; no mouse bindings.

# ENVIRONMENT

*HOME*
	Used to locate the configuration file at *$HOME/.config/labar.cfg*.

*LANG*, *LC_TIME*
	Used by *strftime*(3) to localise day and month names in the date
	slot.

# FILES

*~/.config/labar.cfg*
	User configuration file. Created automatically on first run if it
	does not exist.

*/usr/share/pixmaps/labar/*
	Default directory for volume widget icons
	(*pnmixer-high.png*, *pnmixer-medium.png*, *pnmixer-low.png*,
	*pnmixer-off.png*, *pnmixer-muted.png*).

# EXAMPLES

Run with default settings:
```
labar
```

Run with lifecycle logging:
```
labar -v
```

Run with Wayland protocol and config parsing details:
```
labar -vv
```

Run with full file I/O and cache tracing:
```
labar -vvv
```

Print the version:
```
labar -V
```

# SEE ALSO

*strftime*(3), *alsamixer*(1)

# AUTHORS

Jacques Boscq <jacques@boscq.fr>

Source code available at https://github.com/Amodio/labar
