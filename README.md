# clipling

A popup clipboard picker for Wayland, built on top of
[clipvault](https://github.com/rolv-apneseth/clipvault) history. The window
opens at the mouse cursor: text entries with live search on the left, a grid of
image thumbnails on the right.

- pick an entry with the mouse or keyboard → it goes to the clipboard and/or the
  primary selection → the window closes;
- opens in ~60 ms: the process stays resident, like a daemon;
- GTK CSS themes: calm **dark** / **light** and **pipboy** with a CRT glitch;
- settings window: sizes, text panel width, thumbnails, animations, where to copy.

## How it works

```
Ctrl+C / selection ──wl-paste --watch──▶ clipvault store ──▶ clipvault.db (SQLite)
                                                                   │ read-only
SUPER+V ──▶ clipling ──(D-Bus, GApplication)──▶ resident clipling
                                                   │  window at the cursor
                                                   └─ pick ──▶ wl-copy (+ --primary)
```

- **Storage and capture** are done by clipvault: `wl-paste --watch clipvault store`
  writes every copy into an SQLite database. clipling only reads it.
- **Resident process.** clipling is the GApplication `io.github.serlk.clipling`.
  The first launch starts the process; every later `clipling` call just forwards
  the command to it over D-Bus, so the already built window shows up almost
  instantly. A cold GTK start takes ~0.4 s, so it is best to start the process
  when the session starts (`clipling --daemon`).
- **Lazy reading.** On open, clipling reads the beginning of each entry (label
  and image signature) plus the text used for search; full images are loaded
  only when copied. Thumbnails are decoded in a background thread and cached in
  memory; the list is rebuilt only when the history has changed.
- **The window** is a transparent overlay layer (wlr-layer-shell, namespace
  `clipling`) covering the monitor under the mouse. The panel is drawn below and
  to the right of the cursor, flips near screen edges, and is centered if it
  does not fit. Clicking outside the panel closes the window. Closing on focus
  loss is deliberately not used: with `follow_mouse = 1`, focus moves away as
  soon as the mouse hovers a neighbouring window.
- **Cursor position** comes from `hyprctl cursorpos`. clipling also works in
  other layer-shell compositors (sway, river, niri…), but the window is centered
  on the monitor there.

## Requirements

| | packages (Arch) |
|---|---|
| runtime | `gtkmm-4.0`, `gtk4-layer-shell`, `sqlite`, `wl-clipboard`, `clipvault` (AUR) |
| opening at the cursor | `hyprland` (`hyprctl`) |
| build | `meson`, `ninja`, a C++20 compiler (`gcc`) |

The compositor must support `wlr-layer-shell` (Hyprland, sway, river, niri,
KDE, etc.; GNOME does not).

## Installation

### AUR

```bash
yay -S clipling        # release
yay -S clipling-git    # latest from main
```

### From source

```bash
git clone https://github.com/Serlk/clipling
cd clipling
meson setup build --prefix=/usr
meson compile -C build
sudo meson install -C build
```

Or just for your user, without root:

```bash
meson setup build && meson compile -C build
install -Dm755 build/clipling ~/.local/bin/clipling
```

**After an update**, stop the old process, otherwise the previous version keeps
serving the window: `clipling --quit` (the next call starts the new one).

## Setup

### 1. Let clipvault record history

clipvault should watch both selections: the regular clipboard (Ctrl+C) and the
primary selection (mouse selection). Add to your compositor's autostart:

```bash
wl-paste --watch clipvault store
wl-paste --primary --watch clipvault store
```

The default database is `$XDG_DATA_HOME/clipvault.db` (`~/.local/share/clipvault.db`);
set `CLIPVAULT_DB` to use another path — both clipvault and clipling honour it.

### 2. Hyprland

**Lua config** (`~/.config/hypr/hyprland.lua`):

```lua
hl.on("hyprland.start", function()
    hl.exec_cmd("wl-paste --watch clipvault store")
    hl.exec_cmd("wl-paste --primary --watch clipvault store")
    hl.exec_cmd("clipling --daemon")      -- makes the window open instantly
end)

-- inside your block of SUPER binds
bind("V", exec("clipling"))

-- the clipling theme draws the open animation, not Hyprland
hl.layer_rule({
    name    = "clipling-layer",
    match   = { namespace = "^(clipling)$" },
    no_anim = true,
})

-- settings window
hl.window_rule({
    name  = "clipling-settings",
    match = { class = "^(io\\.github\\.serlk\\.clipling)$" },
    float = true,
})
```

**hyprlang** (`~/.config/hypr/hyprland.conf`):

```ini
exec-once = wl-paste --watch clipvault store
exec-once = wl-paste --primary --watch clipvault store
exec-once = clipling --daemon

bind = SUPER, V, exec, clipling

layerrule  = no_anim on, match:namespace ^(clipling)$
windowrule = float on, match:class ^(io\.github\.serlk\.clipling)$
```

### 3. Other compositors (sway, etc.)

```
exec clipling --daemon
bindsym $mod+v exec clipling
```

The window opens centered on the monitor (the cursor position is only available
through `hyprctl`).

## Usage

```
clipling              show the picker; if it is open, hide it
clipling --settings   settings window
clipling --daemon     start the process without a window (for autostart)
clipling --quit       stop the resident process
```

| key / action | what it does |
|---|---|
| typing | live filter over the full text of entries, case-insensitive (images are hidden while filtering) |
| `Enter` | one match — copies it right away; several — a short "blink" and selects the first |
| `↓` | move from the search field into the list |
| `Enter` / click on an entry | copy and close |
| `Esc`, click outside the panel | close |
| ⚙ | open settings |

## Settings

The gear in the window or `clipling --settings`: theme, window size, **text
panel width** (the rest is images), gap from the cursor, thumbnail size, number
of columns, filter delay, animations, where to copy (primary / clipboard).
Stored in `~/.config/clipling/clipling.ini` and applied the next time the window
opens.

## Themes

On first run clipling creates `~/.config/clipling/themes/{dark,light,pipboy}.css`
(samples are also installed to `/usr/share/clipling/themes/`). The active theme
is chosen in settings or via the `name` key in the `[theme]` section of the config.

- **dark** (default), **light** — calm, Breeze / macOS-like, soft fade-in and
  selection highlight;
- **pipboy** — phosphor-green CRT: turns on like a CRT tube (flash line → expand
  → RGB split, jitter, skew), turns off by collapsing into a line.

Your own theme: copy any `*.css` under a new name into `~/.config/clipling/themes/`,
edit it and pick it in settings. Classes:

| selector | what it is |
|---|---|
| `window.clipling` | the window (transparent, covers the monitor) |
| `.clipling-root` | the panel itself |
| `.clipling-search`, `.clipling-gear` | search field, gear button |
| `.clipling-text row`, `.clipling-row` | text rows |
| `.clipling-images`, `.clipling-tile` | image grid and a tile |
| `.anim-in` / `.anim-out` / `.pulse` | on `window.clipling`: open, close, "blink" |

Animations are plain `@keyframes`; `opacity`, `transform` (translate, scale,
skew), `filter`, `box-shadow` and `text-shadow` are available. The window is
hidden 170 ms after `.anim-out` starts, so the close animation must fit in that time.

clipling's CSS is loaded with a priority above `~/.config/gtk-4.0/gtk.css`, and
the base styles reset the system theme's backgrounds and borders so it does not
show through.

## Limitations

- Opening at the cursor works only in Hyprland.
- The glitch is built from what GTK CSS can do: text is split into colour
  channels via `text-shadow`, but images are not, and there is no line
  tearing; that would need a GSK shader.
- The list shows the first line of an entry (up to 200 characters); search
  covers the full text, but only the first 64 KB of very long entries.

## License

[AGPL-3.0-only](LICENSE).
