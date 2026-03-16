# labar

Lightweight launch bar for Wayland
([unrelated](https://www.youtube.com/watch?v=le-wL4Sm5Xg),
[ditto](https://www.youtube.com/watch?v=C-fXYJj316Q)).

## Context

~20 years ago, I was happy using [wbar](https://github.com/rodolf0/wbar) as a
dock (app launcher) on [Openbox](https://github.com/danakj/openbox).

Now with Wayland replacing X11, I am gladly using
[labwc](https://labwc.github.io).

[lavalauncher](https://git.sr.ht/~leon_plickat/lavalauncher) is the closest
dock to my knowledge, but it also seems discontinued (for 4 years)
and lacks features (like transparency).

## Getting Started

### Compatibility

Your Wayland compositor must implement the
[wlr-protocols](https://gitlab.freedesktop.org/wlroots/wlr-protocols).

Because these protocols are not part of the Wayland core, compositors using
wlroots should be compatible, not GNOME, see:
[wlr-layer-shell-unstable-v1](
https://wayland.app/protocols/wlr-layer-shell-unstable-v1#compositor-support).

Note: a [merge request](
https://gitlab.freedesktop.org/wayland/wayland-protocols/-/merge_requests/28)
is opened (since 2020!) to include `wlr-layer-shell` as an extension.

### Installation from source

```bash
meson setup build/
#meson compile -C build/
sudo meson install -C build/
```

## Help

Contributions (issues/PR) are welcome aswell as handling packages for
distributions!

To install (locally) the pre-commit hook (checks the coding style), run once:
```bash
./scripts/install-hooks.sh
```

## Ideas
 * network activity (up/down speed) widget
 * CPU/RAM usage widget
 * date widget: open a calendar on left clicks
 * volume widget: make `alsa-lib-dev` optional + support `pulseaudio`/`pipewire`
 * xdg-output support (all by default)
