# smpl-scroll

A Hyprland plugin that lets a column of the built-in **scrolling layout** grow wider
than the monitor, the way the workspace itself is wider than the monitor.

Hyprland stops a column at one viewport (the monitor minus the outer gaps): its
resize code caps the column width at `1.0` and also keeps the column's right edge
inside the view. Resizing with `Super+Ctrl+R` and the arrow keys therefore stalls at
the monitor's width. The plugin patches that and keeps the view usable on the wider
column. It is not a layout engine and not the retired `smplscroll` engine: the
columns, rows, focus, animations, gestures and every `layoutmsg` command stay
Hyprland's own.

Without the plugin nothing breaks: columns simply keep Hyprland's limit.

## What changes

| | Hyprland | With smpl-scroll |
|---|---|---|
| Column width limit | 1 viewport | 4 viewports, and at most 8192 logical px (the client's buffer has to stay inside GPU texture limits). Never below Hyprland's own 1 viewport. |
| Resizing the right edge (keyboard `resizeactive`, or dragging) | stalls at the monitor | grows on; the left edge stays where it is and the view stays put |
| Resizing the left edge | | the right edge stays where it is on screen |
| Focusing, clicking or `layoutmsg move +col` on a wider column | centres it, which undoes every scroll within it | the view stays where it is while it is inside the column |
| Focusing a wider column from its left / right neighbour | | shows its start / its end |

Everything else is identical to Hyprland: the minimum column width (5 % of a
viewport), the row heights inside a column (10 % to 100 %), the shrink behaviour, and
a width that is already past a limit is kept, not snapped back.

A single mouse drag is bounded by the pointer, which cannot leave the monitor, so the
keyboard path (`Super+Ctrl+R`, then the arrow keys, which repeat) is the practical way
past the monitor's width.

### Seeing the rest of a wide column

The view does not follow a growing edge. Scroll it:

| Keys | What |
|------|------|
| `Super+Shift+]` / `Super+Shift+[` | Pan the view right / left by 400 px, repeating while held (`layoutmsg move -400` / `move +400`; the sign is Hyprland's: `+N` moves the content right) |
| `Super+]` / `Super+[` | Focus the next / previous column; a wide one is shown from the side you come from |
| `Super+\` | Centre the focused column (this is Hyprland's own centring) |

The view cannot leave the column while it stays focused. Panning past an end moves
focus to the neighbouring column, and Hyprland fits that one as usual.

The preset sizes (`colresize`, `Super+R` "fill", `Super+-`, `Super+=`, `fit active`)
are Hyprland's own commands and give a column at most one viewport, by design: they
bring a wide column back to a normal size. If the plugin is unloaded, columns keep
the width they have until the next resize, where Hyprland clamps them again.

## Source layout

| File | Role |
|------|------|
| `scroll_math.{hpp,cpp}` | Compositor-free arithmetic: the width ceiling, the width and row-height changes of a resize, where the view rests on a wide column, the version check. |
| `main.cpp` | Plugin entry points: ABI check, version gate, the two hooks, cleanup. |
| `globals.hpp` | The plugin handle. |
| `tests/` | Unit tests for `scroll_math`. |

The two hooks, found by their mangled name with `dlsym` and installed with Hyprland's
function-hook API:

| Hyprland function | Replaced by |
|-------------------|-------------|
| `Layout::Tiled::CScrollingAlgorithm::resizeTarget` | `resizeColumn()`: Hyprland's own handling of a window in a column without the two width limits. Windows that are not in a column of a horizontal scrolling layout still go to Hyprland. |
| `Layout::Tiled::CScrollTapeController::fitStrip` | `hkFitStrip()`: for a strip wider than the viewport, keeps the offset inside the strip instead of centring it. Strips that fit still go to Hyprland. |

The hooks are installed together or not at all.

## Hyprland upgrades

The plugin copies the logic of the two functions, so it is verified per Hyprland
series (`kVerifiedHyprland` in `main.cpp`, currently `0.56`). On any other version it
shows one notification, loads nothing and Hyprland behaves as stock. A Hyprland that
changes a function's signature has no matching symbol, so the plugin is skipped too.

On each Hyprland minor release:

1. Diff `CScrollingAlgorithm::resizeTarget` and `CScrollTapeController::fitStrip`
   (`src/layout/algorithm/tiled/scrolling/` upstream) against `resizeColumn()` and
   `hkFitStrip()`. The only differences from upstream should be the width limits
   and the wide-strip branch.
2. Adapt, run `make test`, then try it in a throwaway Hyprland (see below).
3. Bump `kVerifiedHyprland`.

## ABI and delivery

Identical to `smpl-hud`: a Hyprland plugin only loads into the exact build it was
compiled against, so smplOS never ships a prebuilt binary.
`src/shared/bin/smplos-plugins` compiles every plugin directory for the installed
Hyprland (ISO build, Update OS, `release/dev-local.sh plugins`) and the login
autostart loads the matching binaries. See `../smpl-hud/README.md` for the table, and
`smplos-plugins status` / `reload` for the current state.

## Build and test

```bash
make            # build/smpl-scroll.so against the installed Hyprland
make test       # unit tests; plain g++, no Hyprland headers, runs anywhere
make clean
```

`make` needs the Hyprland development headers (the `hyprland` package ships them),
the libraries `pkg-config` looks for, `base-devel` and a C++23 compiler. CI runs
`make test` on every change to this directory.

A plugin fault takes the whole compositor, and every open window, down. Try changes
in a throwaway Hyprland instance (a VM or a nested/headless instance), not in the
session you work in, and load a build by path with
`hyprctl plugin load "$PWD/build/smpl-scroll.so"`. With a `.conf` config in that
instance, resize with `hyprctl dispatch resizeactive 400 0` and pan with
`hyprctl dispatch layoutmsg "move -400"`.

## Limits

- Only the horizontal scrolling layout: a vertical tape (`direction up/down`) is
  left to Hyprland.
- Touchpad scrolling gestures still snap a wider column to its centre (Hyprland's
  gesture snapping is not hookable). smplOS binds none.
- A client has to cope with a window of this size; very large buffers cost memory.
