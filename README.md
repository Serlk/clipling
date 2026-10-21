# clipling

Всплывающий буфер обмена для Wayland поверх истории
[clipvault](https://github.com/rolv-apneseth/clipvault): окно появляется у
курсора, слева — текст с живым поиском, справа — сетка превью картинок.

*Popup clipboard picker for Wayland on top of clipvault history: opens at the
cursor, text on the left with live search, image thumbnails on the right.
GTK4 + layer-shell, resident process for instant (~60 ms) opening.*

- выбор мышью или клавиатурой → запись кладётся в буфер (clipboard и/или
  primary) → окно закрывается;
- открывается за ~60 мс — процесс живёт в памяти, как демон;
- темы на GTK CSS: спокойные **dark** / **light** и **pipboy** с CRT-глитчем;
- окно настроек: размеры, ширина панели текста, превью, анимации, куда копировать.

## Как это работает

```
Ctrl+C / выделение ──wl-paste --watch──▶ clipvault store ──▶ clipvault.db (SQLite)
                                                                   │ read-only
SUPER+V ──▶ clipling ──(D-Bus, GApplication)──▶ резидентный clipling
                                                   │  окно у курсора
                                                   └─ выбор ──▶ wl-copy (+ --primary)
```

- **Хранение и перехват** делает clipvault: `wl-paste --watch clipvault store`
  пишет каждое копирование в SQLite-базу. clipling её только читает.
- **Резидентный процесс.** clipling — GApplication `io.github.serlk.clipling`.
  Первый запуск поднимает процесс, каждый следующий `clipling` лишь пересылает
  ему команду по D-Bus, и уже готовое окно показывается почти мгновенно.
  Холодный старт GTK — ~0.4 с, поэтому процесс лучше поднять при входе в сессию
  (`clipling --daemon`).
- **Ленивое чтение.** При открытии из базы берётся только начало каждой записи
  (подпись и сигнатура картинки), содержимое целиком — при копировании.
  Превью картинок декодируются в фоновом потоке и кэшируются в памяти; список
  пересобирается, только если история изменилась.
- **Окно** — прозрачный overlay-слой (wlr-layer-shell, namespace `clipling`) на
  весь монитор под мышью; панель рисуется снизу-справа от курсора, у краёв
  экрана переворачивается, не влезает — по центру. Клик мимо панели закрывает
  окно. Закрытие по потере фокуса не используется намеренно: при
  `follow_mouse = 1` фокус уходит от простого наведения на соседнее окно.
- **Позиция курсора** — из `hyprctl cursorpos`. В других композиторах с
  layer-shell (sway, river, niri…) clipling тоже работает, но окно встаёт по
  центру монитора.

## Что нужно

| | пакеты (Arch) |
|---|---|
| рантайм | `gtkmm-4.0`, `gtk4-layer-shell`, `sqlite`, `wl-clipboard`, `clipvault` (AUR) |
| позиционирование у курсора | `hyprland` (`hyprctl`) |
| сборка | `meson`, `ninja`, компилятор C++20 (`gcc`) |

Композитор должен поддерживать `wlr-layer-shell` (Hyprland, sway, river, niri,
KDE и др.; GNOME — нет).

## Установка

### AUR

```bash
yay -S clipling        # релиз
yay -S clipling-git    # последняя версия из main
```

### Из исходников

```bash
git clone https://github.com/Serlk/clipling
cd clipling
meson setup build --prefix=/usr
meson compile -C build
sudo meson install -C build
```

Или только для себя, без root:

```bash
meson setup build && meson compile -C build
install -Dm755 build/clipling ~/.local/bin/clipling
```

**После обновления** завершите старый процесс, иначе окно будет показывать
прежняя версия: `clipling --quit` (следующий вызов поднимет новую).

## Настройка

### 1. clipvault пишет историю

clipvault должен слушать оба буфера — обычный (Ctrl+C) и primary (выделение
мышью). В автозапуск композитора:

```bash
wl-paste --watch clipvault store
wl-paste --primary --watch clipvault store
```

База по умолчанию — `$XDG_DATA_HOME/clipvault.db` (`~/.local/share/clipvault.db`);
другой путь задаётся переменной `CLIPVAULT_DB` — её понимают и clipvault, и clipling.

### 2. Hyprland

**Lua-конфиг** (`~/.config/hypr/hyprland.lua`):

```lua
hl.on("hyprland.start", function()
    hl.exec_cmd("wl-paste --watch clipvault store")
    hl.exec_cmd("wl-paste --primary --watch clipvault store")
    hl.exec_cmd("clipling --daemon")      -- окно будет открываться мгновенно
end)

-- внутри блока биндов с SUPER
bind("V", exec("clipling"))

-- анимацию появления рисует тема clipling, а не Hyprland
hl.layer_rule({
    name    = "clipling-layer",
    match   = { namespace = "^(clipling)$" },
    no_anim = true,
})

-- окно настроек
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

### 3. Другие композиторы (sway и т.п.)

```
exec clipling --daemon
bindsym $mod+v exec clipling
```

Окно откроется по центру монитора (позиция курсора доступна только через `hyprctl`).

## Использование

```
clipling              показать окно выбора; если открыто — спрятать
clipling --settings   окно настроек
clipling --daemon     поднять процесс без окна (для автозапуска)
clipling --quit       завершить резидентный процесс
```

| клавиша / действие | что делает |
|---|---|
| ввод текста | живой фильтр по тексту (картинки при фильтре скрываются) |
| `Enter` | одно совпадение — сразу копирует; несколько — «подмиг» и выбор первого |
| `↓` | из поиска в список |
| `Enter` / клик по записи | скопировать и закрыть |
| `Esc`, клик мимо панели | закрыть |
| ⚙ | открыть настройки |

## Настройки

Шестерёнка в окне или `clipling --settings`: тема, размер окна, **ширина
текстовой панели** (остальное — картинки), отступ от курсора, размер превью,
число колонок, задержка фильтра, анимации, куда копировать (primary /
clipboard). Хранятся в `~/.config/clipling/clipling.ini` и применяются при
следующем открытии окна.

## Темы

При первом запуске создаются `~/.config/clipling/themes/{dark,light,pipboy}.css`
(образцы также лежат в `/usr/share/clipling/themes/`). Активная тема — в
настройках или строка `name` в секции `[theme]` конфига.

- **dark** (по умолчанию), **light** — спокойные, в духе Breeze / macOS, мягкое
  появление и подсветка выбора;
- **pipboy** — фосфорно-зелёный CRT: включение как у ЭЛТ (вспышка-полоса →
  раскрытие → RGB-сдвиг, рывки, перекос), выключение схлопыванием в линию.

Своя тема: скопируйте любой `*.css` под новым именем в `~/.config/clipling/themes/`,
правьте и выберите в настройках. Классы:

| селектор | что это |
|---|---|
| `window.clipling` | окно (прозрачное, на весь монитор) |
| `.clipling-root` | сама панель |
| `.clipling-search`, `.clipling-gear` | поле поиска, шестерёнка |
| `.clipling-text row`, `.clipling-row` | строки текста |
| `.clipling-images`, `.clipling-tile` | сетка картинок и плитка |
| `.anim-in` / `.anim-out` / `.pulse` | на `window.clipling`: появление, закрытие, «подмиг» |

Анимации — обычные `@keyframes`; доступны `opacity`, `transform` (translate,
scale, skew), `filter`, `box-shadow`, `text-shadow`. Окно прячется через 170 мс
после начала `.anim-out` — закрывающая анимация должна укладываться в это время.

CSS clipling подключается с приоритетом выше `~/.config/gtk-4.0/gtk.css`, а
базовые стили гасят фон и рамки системной темы, чтобы она не просвечивала.

## Ограничения

- Позиция у курсора — только в Hyprland.
- Глитч собран из возможностей GTK CSS: текст расслаивается по цветам через
  `text-shadow`, но картинки — нет, и нет «разрыва» по строкам; для этого нужен
  GSK-шейдер.
- Поиск идёт по первой строке записи.

## Лицензия

[AGPL-3.0-only](LICENSE).
