# labar

Lightweight Wayland bar — app launcher, network speed, CPU/RAM usage, volume,
and date/time widgets
([unrelated](https://www.youtube.com/watch?v=le-wL4Sm5Xg),
[ditto](https://www.youtube.com/watch?v=C-fXYJj316Q)).

## Context

~20 years ago, I was happy using [wbar](https://github.com/rodolf0/wbar) as a
dock (app launcher) on [Openbox](https://github.com/danakj/openbox).
Now with Wayland replacing X11, I am gladly using
[labwc](https://labwc.github.io).

[lavalauncher](https://git.sr.ht/~leon_plickat/lavalauncher) is the closest
dock to my knowledge, but it seems discontinued (4 years) and lacks features.

## Features

- App launcher — click to launch, hover label, SVG/PNG icons
- Network speed widget — live ↓/↑ KB/MB/GB/s from `/proc/net/dev`
- CPU/RAM usage widget — live percentages from `/proc/stat` & `/proc/meminfo`
- Volume widget — ALSA PCM/Master, mute toggle, scroll to adjust
- Date/time widget — localization, click to open a calendar popup
- Locale-aware calendar popup — localization, rendered inline via Cairo
- HiDPI — integer scaling via `wl_output.scale`, fractional via
  `wp_fractional_scale_v1`
- Output selection — pin the bar to a specific monitor by name
- Graphical config editor — GTK4, reorderable widgets, live preview (optional)
- Positioned on any screen edge: top, bottom, left, right

## Compatibility

labar requires a Wayland compositor that implements
[wlr-layer-shell-unstable-v1](
  https://wayland.app/protocols/wlr-layer-shell-unstable-v1#compositor-support).
Compositors based on [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots)
(labwc, Sway, Hyprland, river, etc.) are compatible. GNOME or KDE are not.

### Wayland protocols used

| Protocol | Source |
|---|---|
| `xdg-shell` | wayland-protocols stable |
| `viewporter` | wayland-protocols stable |
| `fractional-scale-v1` | wayland-protocols staging |
| `xdg-output-unstable-v1` | wayland-protocols unstable |
| `wlr-layer-shell-unstable-v1` | wlr-protocols (bundled) |

> **Note:** a [merge request](
  https://gitlab.freedesktop.org/wayland/wayland-protocols/-/merge_requests/28)
> has been open since 2020 to include `wlr-layer-shell` in the upstream
> wayland-protocols repository.

## Dependencies

| Library | Purpose |
|---|---|
| `wayland-client` | Wayland protocol |
| `wayland-protocols` | Protocol XML files |
| `cairo` | Tile rendering |
| `librsvg-2.0` | SVG icon loading |
| `alsa-lib` | Volume widget |
| `gtk4` *(optional)* | Graphical config editor (`--config`) |

## Installation

```bash
meson setup build/
#meson compile -C build/
sudo meson install -C build/
```

The GTK4 config editor is built automatically if `gtk4` is found.

## Usage

```
labar [OPTIONS]
```

| Option | Description |
|---|---|
| `-h`, `--help` | Show help and exit |
| `-v` ... `-vvvv` | Increase verbosity (up to 4 levels) |
| `-c`, `--config` | Open the graphical config editor |
| `-V`, `--version` | Print version and exit |

The configuration file is read from `~/.config/labar.cfg` and is created with
sensible defaults on first run. Run `labar -v` to see which output names are
available for the `output=` key.

## Contributing

Issues and pull requests are welcome, as well as packaging for distributions!

To install the pre-commit hook (coding style check) locally, run once:
```bash
./scripts/install-hooks.sh
```

## Ideas
 * volume widget: make `alsa-lib-dev` optional, support `pulseaudio`
and/or `pipewire`.
