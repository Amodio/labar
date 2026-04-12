# labar

Wayland bar — app launcher, network speed, CPU/RAM usage, volume,
and date/time widgets
([unrelated](https://www.youtube.com/watch?v=le-wL4Sm5Xg),
[ditto](https://www.youtube.com/watch?v=C-fXYJj316Q)).

![Preview](screenshot.png "Preview")

## Context

~20 years ago, I was happy using [wbar](https://github.com/rodolf0/wbar) as a
dock (app launcher) on [Openbox](https://github.com/danakj/openbox).
Now with Wayland replacing X11, I am gladly using
[labwc](https://labwc.github.io).

[lavalauncher](https://git.sr.ht/~leon_plickat/lavalauncher) is the closest
dock to my knowledge, but it is discontinued (since 2021) and lacks features.

## Features

- App launcher — click to launch, hover label, SVG/PNG icons
- Network speed widget — live ↓/↑ KB/MB/GB/s from `/proc/net/dev`
- CPU/RAM usage widget — live percentages from `/proc/stat` & `/proc/meminfo`
- Volume widget — ALSA PCM/Master, mute toggle, scroll to adjust
- Date/time widget — localization, click to open a calendar popup
- Calendar popup — localization, rendered inline via Cairo
- HiDPI — integer scaling via `wl_output.scale`, fractional via
  `wp_fractional_scale_v1`
- Output selection — pin the bar to a specific monitor by name
- Graphical config editor — GTK4, reorderable widgets, live preview (optional)
- Positioned on any screen edge: top, bottom, left, right

## Compatibility

Compositors based on [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots)
(labwc, Sway, Hyprland, river, etc.) are compatible.

KWin (KDE Plasma) is also compatible, but **[not GNOME](
https://wayland.app/protocols/wlr-layer-shell-unstable-v1#compositor-support)**.

### Wayland protocols used

| Protocol        | Source         |
|----------------|----------------|
| [xdg-shell]    | wayland stable |
| [viewporter]   | wayland stable |
| [fractional-scale-v1] | wayland staging |
| [xdg-output-unstable-v1] | wayland unstable |
| [wlr-layer-shell-v1] | wlr bundled |

[xdg-shell]: https://wayland.app/protocols/xdg-shell
[viewporter]: https://wayland.app/protocols/viewporter
[fractional-scale-v1]: https://wayland.app/protocols/fractional-scale-v1
[xdg-output-unstable-v1]: https://wayland.app/protocols/xdg-output-unstable-v1
[wlr-layer-shell-v1]: https://wayland.app/protocols/wlr-layer-shell-unstable-v1

> **Note:** a [merge request](
  https://gitlab.freedesktop.org/wayland/wayland-protocols/-/merge_requests/28)
> has been open (since 2020!) to include `wlr-layer-shell` in the upstream
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

On Debian:
```bash
sudo apt install meson pkgconf cmake libwayland-dev wayland-protocols \
libcairo2-dev librsvg2-dev libasound2-dev scdoc clang
sudo apt install libgtk-4-dev # if you want the --config option
```

On Alpine Linux:
```bash
sudo apk add meson pkgconf cmake wayland-dev wayland-protocols cairo-dev \
librsvg-dev alsa-lib-dev scdoc clang22-dev compiler-rt
sudo apk add gtk4.0-dev # if you want the --config option
```

## Installation

```bash
meson setup build/ --buildtype=release -Db_sanitize=none
sudo meson install -C build/
```

## For developers

```bash
CC=clang meson setup build-debug/ -Db_lundef=false
meson compile -C build-debug/
sudo meson install -C build-debug/
```

Sanitizers ([Address](https://clang.llvm.org/docs/AddressSanitizer.html),
[Leak](https://clang.llvm.org/docs/LeakSanitizer.html),
[Undefined Behavior](
https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html))
will be embedded in the binary, as well as debug symbols.

You may combine below env. variables:
```bash
ASAN_SYMBOLIZER_PATH=/usr/lib/llvm22/bin/llvm-symbolizer             \
ASAN_OPTIONS=check_initialization_order=1                            \
LSAN_OPTIONS=suppressions=lsan.supp:report_objects=1:use_unaligned=1 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ./build-debug/labar
```

#### `ASAN_OPTIONS`

| Variable | Default | Description |
|---|---|---|
| `detect_leaks` | `1` on Linux | Enable LeakSanitizer |
| `detect_stack_use_after_return` | `1` on Linux | Detect stack use after ret |
| `check_initialization_order` | `0` | Detect dynamic init order issues |
| `suppressions` | — | Path to suppression file |
| `symbolize` | `1` | Symbolize reports (disable with `0` for offline use) |
```bash
ASAN_OPTIONS=detect_leaks=1:check_initialization_order=1:\
suppressions=asan.supp labar
```

Suppression file format:
```
interceptor_via_fun:NameOfCFunctionToSuppress
interceptor_via_lib:NameOfTheLibraryToSuppress
```

#### `LSAN_OPTIONS`

| Variable | Default | Description |
|---|---|---|
| `exitcode` | `23` | When leaks are detected (can differ from ASan's) |
| `max_leaks` | `0` | If non-zero, report only this many top leaks |
| `suppressions` | — | Path to suppression file |
| `print_suppressions` | `1` | Print statistics for matched suppressions |
| `report_objects` | `0` | Report addresses of individual leaked objects |
| `use_unaligned` | `0` | Also scan unaligned 8-byte patterns for pointers |
```bash
LSAN_OPTIONS=suppressions=lsan.supp:report_objects=1:use_unaligned=1 labar
```

#### `UBSAN_OPTIONS`

| Variable | Default | Description |
|---|---|---|
| `print_stacktrace` | `0` | Print stack trace on each error |
| `halt_on_error` | `0` | Stop on first error (like ASan) |
| `suppressions` | — | Path to suppression file |
```bash
UBSAN_OPTIONS=suppressions=ubsan.supp:print_stacktrace=1:halt_on_error=1 labar
```

For readable stack traces across all sanitizers, ensure `llvm-symbolizer` is 
in your `$PATH`, or set:
```bash
ASAN_SYMBOLIZER_PATH=/usr/lib/llvm22/bin/llvm-symbolizer
```

## Usage

```bash
labar [OPTIONS]
```

| Option | Description |
|---|---|
| `-h`, `--help` | Show help and exit |
| `-v` ... `-vvvv` | Increase verbosity (up to 4 levels) |
| `-c`, `--config` | Open the graphical config editor |
| `-V`, `--version` | Print version and exit |

The configuration file is read from `~/.config/labar.cfg`.
You can set your `LC_TIME` env var to a value from `locale -a` like that:
```bash
LC_TIME=fr_FR.UTF-8 labar
```

## Contributing

Issues and pull requests are welcome, as well as packaging for distributions!

To use the pre-commit hook locally (coding style checks), run once:
```bash
./scripts/install-hooks.sh
```
