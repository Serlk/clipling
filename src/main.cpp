// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 serlk
//
// clipling — a popup clipboard picker on top of clipvault history.
//
// clipvault does the capturing and storage; clipling reads its SQLite database
// (read-only) and draws a window at the cursor: text with live search on the
// left, a grid of image thumbnails on the right. Pick -> wl-copy (primary +
// clipboard) -> the window closes. Look and effects live in GTK CSS (themes).
//
// Runs resident: the first launch starts the process, later ones just ask it
// to show the window (GApplication) — that is what makes opening instant, like rofi.
//
// Build: meson (see meson.build). Dependencies: gtkmm-4.0, gtk4-layer-shell, sqlite3.

#include <gtkmm.h>
#include <glibmm.h>
#include <giomm.h>
#include <glib.h>
#include <gtk4-layer-shell.h>
#include <sqlite3.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

// =====================================================================
//  Config (stored in ~/.config/clipling/clipling.ini via GKeyFile)
// =====================================================================

struct Config {
    int  width       = 820;
    int  height      = 480;
    int  split       = 0;    // text panel width, px (0 = auto, ~58%)
    int  gap         = 14;   // gap between the window and the cursor
    int  thumb       = 120;  // thumbnail size, px
    int  image_cols  = 6;    // max columns in the image grid
    int  debounce_ms = 120;  // live filter delay
    bool animations  = true;
    bool copy_primary   = true;
    bool copy_clipboard = true;
    std::string theme = "dark";
};

static fs::path config_dir()  { return fs::path(g_get_user_config_dir()) / "clipling"; }
static fs::path config_file() { return config_dir() / "clipling.ini"; }
static fs::path themes_dir()  { return config_dir() / "themes"; }

static Config load_config() {
    Config c;
    auto kf = Glib::KeyFile::create();
    try { if (!kf->load_from_file(config_file().string())) return c; }
    catch (...) { return c; }
    auto gi = [&](const char* g, const char* k, int d)  { try { return kf->has_key(g,k)? kf->get_integer(g,k): d; } catch(...) { return d; } };
    auto gb = [&](const char* g, const char* k, bool d) { try { return kf->has_key(g,k)? kf->get_boolean(g,k): d; } catch(...) { return d; } };
    auto gs = [&](const char* g, const char* k, const std::string& d) { try { return kf->has_key(g,k)? std::string(kf->get_string(g,k)): d; } catch(...) { return d; } };
    c.width         = gi("window","width",c.width);
    c.height        = gi("window","height",c.height);
    c.split         = gi("window","split",c.split);
    c.gap           = gi("window","gap",c.gap);
    c.thumb         = gi("behavior","thumb",c.thumb);
    c.image_cols    = gi("behavior","image_cols",c.image_cols);
    c.debounce_ms   = gi("behavior","debounce_ms",c.debounce_ms);
    c.animations    = gb("effects","animations",c.animations);
    c.copy_primary  = gb("copy","primary",c.copy_primary);
    c.copy_clipboard= gb("copy","clipboard",c.copy_clipboard);
    c.theme         = gs("theme","name",c.theme);
    return c;
}

static void save_config(const Config& c) {
    auto kf = Glib::KeyFile::create();
    kf->set_integer("window","width",c.width);
    kf->set_integer("window","height",c.height);
    kf->set_integer("window","split",c.split);
    kf->set_integer("window","gap",c.gap);
    kf->set_integer("behavior","thumb",c.thumb);
    kf->set_integer("behavior","image_cols",c.image_cols);
    kf->set_integer("behavior","debounce_ms",c.debounce_ms);
    kf->set_boolean("effects","animations",c.animations);
    kf->set_boolean("copy","primary",c.copy_primary);
    kf->set_boolean("copy","clipboard",c.copy_clipboard);
    kf->set_string ("theme","name",c.theme);
    std::error_code ec; fs::create_directories(config_dir(), ec);
    std::ofstream(config_file()) << std::string(kf->to_data());
}

// =====================================================================
//  History data
// =====================================================================

// On open, only the start of an entry (label, image signature) and the text for
// search are read; a full image is read when copying and when decoding thumbnails.
struct Entry {
    int64_t     id = 0;
    std::string mime;   // empty -> text
    std::string text;   // short label (first line)
    std::string hay;    // full text (up to SEARCH_LIMIT), passed through fold_text()
};

static constexpr int SEARCH_LIMIT = 64 * 1024;   // no point searching further into huge entries

static std::string sniff_image_mime(const std::vector<uint8_t>& b) {
    auto has = [&](std::initializer_list<uint8_t> sig, size_t off = 0) {
        if (b.size() < off + sig.size()) return false;
        size_t i = off; for (uint8_t c : sig) if (b[i++] != c) return false; return true;
    };
    if (has({0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A})) return "image/png";
    if (has({0xFF,0xD8,0xFF}))                       return "image/jpeg";
    if (has({'B','M'}))                              return "image/bmp";
    if (has({'G','I','F','8','7','a'}) || has({'G','I','F','8','9','a'})) return "image/gif";
    if (has({'R','I','F','F'}) && has({'W','E','B','P'}, 8)) return "image/webp";
    return {};
}

static std::string valid_utf8(const char* data, size_t n) {
    char* valid = g_utf8_make_valid(data, (gssize)n);
    std::string out = valid ? valid : ""; g_free(valid); return out;
}

static std::string first_line_utf8(const std::vector<uint8_t>& content) {
    std::string raw(content.begin(), content.end());
    if (auto nl = raw.find('\n'); nl != std::string::npos) raw.resize(nl);
    std::string line = valid_utf8(raw.data(), raw.size());
    if (g_utf8_strlen(line.c_str(), -1) > 200) {          // cut by characters, not bytes
        char* cut = g_utf8_substring(line.c_str(), 0, 200);
        line = cut; g_free(cut);
    }
    return line;
}

// Case-insensitive matching for any alphabet (ascii tolower left Cyrillic untouched).
static std::string fold_text(const std::string& s) {
    char* norm = g_utf8_normalize(s.c_str(), (gssize)s.size(), G_NORMALIZE_DEFAULT);
    if (!norm) return s;
    char* folded = g_utf8_casefold(norm, -1);
    std::string out = folded ? folded : s;
    g_free(norm); g_free(folded);
    return out;
}

static std::string clipvault_db_path() {
    if (const char* env = g_getenv("CLIPVAULT_DB")) return env;
    return std::string(g_get_user_data_dir()) + "/clipvault.db";   // same as clipvault: $XDG_DATA_HOME
}

static sqlite3* open_db() {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(clipvault_db_path().c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return nullptr;
    }
    return db;
}

static std::vector<Entry> load_entries() {
    std::vector<Entry> out;
    sqlite3* db = open_db();
    if (!db) return out;
    // The first 1024 bytes are usually enough; long text is read separately
    // so that full images are not pulled from the database.
    sqlite3_stmt* st = nullptr;
    sqlite3_stmt* more = nullptr;
    sqlite3_prepare_v2(db, "SELECT substr(content, 1, ?) FROM clipboard WHERE id = ?", -1, &more, nullptr);
    if (sqlite3_prepare_v2(db, "SELECT id, substr(content, 1, 1024), length(content) FROM clipboard ORDER BY last_updated DESC",
                           -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const auto* blob = (const uint8_t*)sqlite3_column_blob(st, 1);
            int n = sqlite3_column_bytes(st, 1);
            if (!blob || n <= 0) continue;
            std::vector<uint8_t> head(blob, blob + n);
            Entry e;
            e.id = sqlite3_column_int64(st, 0);
            e.mime = sniff_image_mime(head);
            if (e.mime.empty()) {
                e.text = first_line_utf8(head);
                std::string full((const char*)blob, n);
                if (sqlite3_column_int64(st, 2) > n && more) {
                    sqlite3_reset(more);
                    sqlite3_bind_int(more, 1, SEARCH_LIMIT);
                    sqlite3_bind_int64(more, 2, e.id);
                    if (sqlite3_step(more) == SQLITE_ROW)
                        full.assign((const char*)sqlite3_column_blob(more, 0), sqlite3_column_bytes(more, 0));
                }
                e.hay = fold_text(valid_utf8(full.data(), full.size()));
            }
            out.push_back(std::move(e));
        }
    }
    if (st) sqlite3_finalize(st);
    if (more) sqlite3_finalize(more);
    sqlite3_close(db);
    return out;
}

// History fingerprint: if unchanged, the widget list is not rebuilt.
static std::string history_stamp() {
    std::string out;
    sqlite3* db = open_db();
    if (!db) return out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT count(*), max(last_updated), max(id) FROM clipboard", -1, &st, nullptr) == SQLITE_OK
        && sqlite3_step(st) == SQLITE_ROW)
        out = Glib::ustring::compose("%1:%2:%3", sqlite3_column_int64(st, 0),
                                     sqlite3_column_int64(st, 1), sqlite3_column_int64(st, 2));
    if (st) sqlite3_finalize(st);
    sqlite3_close(db);
    return out;
}

static std::vector<uint8_t> load_content(sqlite3* db, int64_t id) {
    std::vector<uint8_t> out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT content FROM clipboard WHERE id = ?", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const auto* blob = (const uint8_t*)sqlite3_column_blob(st, 0);
            int n = sqlite3_column_bytes(st, 0);
            if (blob && n > 0) out.assign(blob, blob + n);
        }
    }
    if (st) sqlite3_finalize(st);
    return out;
}

static void wl_copy_once(const std::vector<uint8_t>& data, const std::string& mime, bool primary) {
    std::string cmd = "wl-copy";
    if (primary) cmd += " --primary";
    if (!mime.empty()) cmd += " --type " + mime;   // mime comes from a whitelist -> safe
    FILE* p = popen(cmd.c_str(), "w");
    if (!p) return;
    if (!data.empty()) fwrite(data.data(), 1, data.size(), p);
    pclose(p);
}

static void copy_entry(const Entry& e, const Config& cfg) {
    sqlite3* db = open_db();
    if (!db) return;
    auto content = load_content(db, e.id);
    sqlite3_close(db);
    if (content.empty()) return;
    if (cfg.copy_clipboard) wl_copy_once(content, e.mime, false);
    if (cfg.copy_primary)   wl_copy_once(content, e.mime, true);
}

// =====================================================================
//  Themes / CSS
// =====================================================================

// Our provider sits above the user's ~/.config/gtk-4.0/gtk.css, but the system
// theme still paints properties clipling themes don't set (list backgrounds,
// borders, shadows) — they are reset explicitly here.
static const char* BASE_CSS = R"CSS(
window.clipling        { background: transparent; border: none; box-shadow: none; border-radius: 0; outline: none; }
window.clipling list, window.clipling flowbox, window.clipling flowboxchild, window.clipling row,
window.clipling scrolledwindow, window.clipling viewport { background: transparent; color: inherit; box-shadow: none; }
window.clipling entry  { box-shadow: none; outline: none; }
window.clipling.pre .clipling-root { opacity: 0; }
.clipling-root         { margin: 6px; padding: 12px; border-radius: 12px; }
.clipling-header       { margin-bottom: 10px; }
.clipling-search       { }
.clipling-gear         { min-width: 26px; min-height: 26px; padding: 2px 6px; }
.clipling-images       { padding: 4px; }
.clipling-tile         { padding: 4px; margin: 4px; border-radius: 8px; }
.clipling-row          { padding: 8px 12px; border-radius: 6px; }
.clipling-settings     { padding: 20px; }
.clipling-settings label.title { font-weight: 700; font-size: 15px; }
.clipling-settings .caption    { opacity: 0.6; font-size: 12px; }
)CSS";

static const char* DARK_CSS = R"CSS(
@define-color bg     #1b1e24;
@define-color fg     #e2e6ec;
@define-color sub    rgba(226,230,236,0.55);
@define-color acc    #3daee9;
@define-color sel    rgba(61,174,233,0.22);
@define-color bord   rgba(255,255,255,0.09);

window.clipling { font-family: "Inter","SF Pro Display",system-ui,sans-serif; font-size: 13px; }
.clipling-root { background-color:@bg; color:@fg; border:1px solid @bord; box-shadow:0 14px 44px rgba(0,0,0,0.5); }
.clipling-search { background:rgba(255,255,255,0.05); color:@fg; border:1px solid @bord; border-radius:8px; padding:6px 10px; caret-color:@acc; }
.clipling-search:focus-within { border-color:@acc; }
.clipling-text row:hover { background:rgba(255,255,255,0.05); }
.clipling-text row:selected, .clipling-text row:active { background:@sel; }
.clipling-tile:hover { background:rgba(255,255,255,0.05); }
.clipling-tile:selected, .clipling-tile:active { background:@sel; }
paned > separator { background:@bord; min-width:1px; }
.clipling-gear { background:transparent; color:@sub; border:none; border-radius:8px; }
.clipling-gear:hover { background:rgba(255,255,255,0.07); color:@fg; }
button { background:rgba(255,255,255,0.06); color:@fg; border:1px solid @bord; border-radius:8px; padding:6px 14px; }
button:hover { background:rgba(255,255,255,0.10); }
button.suggested-action { background:@acc; color:#081019; border-color:transparent; }

@keyframes fade-in  { from{opacity:0; transform:translateY(-6px) scale(0.97);} to{opacity:1; transform:none;} }
@keyframes fade-out { from{opacity:1; transform:none;} to{opacity:0; transform:scale(0.97);} }
@keyframes soft-pulse {
  0%   { box-shadow:0 14px 44px rgba(0,0,0,0.5), inset 0 0 0 1px rgba(61,174,233,0.0); }
  50%  { box-shadow:0 14px 44px rgba(0,0,0,0.5), inset 0 0 0 2px rgba(61,174,233,0.6); }
  100% { box-shadow:0 14px 44px rgba(0,0,0,0.5), inset 0 0 0 1px rgba(61,174,233,0.0); }
}
window.clipling.anim-in  .clipling-root { animation: fade-in  110ms ease-out 1 both; }
window.clipling.anim-out .clipling-root { animation: fade-out 100ms ease-in 1 both; }
window.clipling.pulse    .clipling-root { animation: soft-pulse 220ms ease-in-out 1; }
)CSS";

static const char* LIGHT_CSS = R"CSS(
@define-color bg     #fbfbfc;
@define-color fg     #202327;
@define-color sub    rgba(32,35,39,0.55);
@define-color acc    #2d7fd6;
@define-color sel    rgba(45,127,214,0.16);
@define-color bord   rgba(0,0,0,0.10);

window.clipling { font-family: "Inter","SF Pro Display",system-ui,sans-serif; font-size: 13px; }
.clipling-root { background-color:@bg; color:@fg; border:1px solid @bord; box-shadow:0 12px 36px rgba(0,0,0,0.18); }
.clipling-search { background:#ffffff; color:@fg; border:1px solid @bord; border-radius:8px; padding:6px 10px; caret-color:@acc; }
.clipling-search:focus-within { border-color:@acc; }
.clipling-text row:hover { background:rgba(0,0,0,0.04); }
.clipling-text row:selected, .clipling-text row:active { background:@sel; }
.clipling-tile:hover { background:rgba(0,0,0,0.04); }
.clipling-tile:selected, .clipling-tile:active { background:@sel; }
paned > separator { background:@bord; min-width:1px; }
.clipling-gear { background:transparent; color:@sub; border:none; border-radius:8px; }
.clipling-gear:hover { background:rgba(0,0,0,0.06); color:@fg; }
button { background:#ffffff; color:@fg; border:1px solid @bord; border-radius:8px; padding:6px 14px; }
button:hover { background:rgba(0,0,0,0.04); }
button.suggested-action { background:@acc; color:#ffffff; border-color:transparent; }

@keyframes fade-in  { from{opacity:0; transform:translateY(-6px) scale(0.97);} to{opacity:1; transform:none;} }
@keyframes fade-out { from{opacity:1; transform:none;} to{opacity:0; transform:scale(0.97);} }
@keyframes soft-pulse {
  0%   { box-shadow:0 12px 36px rgba(0,0,0,0.18), inset 0 0 0 1px rgba(45,127,214,0.0); }
  50%  { box-shadow:0 12px 36px rgba(0,0,0,0.18), inset 0 0 0 2px rgba(45,127,214,0.55); }
  100% { box-shadow:0 12px 36px rgba(0,0,0,0.18), inset 0 0 0 1px rgba(45,127,214,0.0); }
}
window.clipling.anim-in  .clipling-root { animation: fade-in  110ms ease-out 1 both; }
window.clipling.anim-out .clipling-root { animation: fade-out 100ms ease-in 1 both; }
window.clipling.pulse    .clipling-root { animation: soft-pulse 220ms ease-in-out 1; }
)CSS";

static const char* PIPBOY_CSS = R"CSS(
window.clipling { color: rgba(140,255,185,0.92); font-family: "CodeNewRoman Nerd Font Propo", monospace; font-size:13px; }
.clipling-root {
  background-color: rgba(3,9,6,0.97);
  background-image: repeating-linear-gradient(to bottom, rgba(0,0,0,0) 0px, rgba(0,0,0,0) 2px, rgba(0,25,10,0.35) 3px);
  border:1px solid rgba(100,255,155,0.22);
  border-radius: 2px;
  box-shadow: inset 0 0 60px rgba(0,35,14,0.75), 0 0 22px rgba(0,255,120,0.12);
  text-shadow: 0 0 6px rgba(80,255,140,0.35);
}
.clipling-row, .clipling-tile { border-radius: 0; }
.clipling-search { background:rgba(0,20,10,0.6); color:rgba(180,255,210,0.95); border:1px solid rgba(100,255,155,0.3); border-radius:2px; }
.clipling-text row:hover { background:rgba(100,255,155,0.10); }
.clipling-text row:selected,.clipling-text row:active { background:rgba(100,255,155,0.20); }
.clipling-tile:hover { background:rgba(100,255,155,0.14); }
.clipling-tile:selected,.clipling-tile:active { background:rgba(100,255,155,0.22); }
paned>separator { background:rgba(100,255,155,0.18); min-width:2px; }
.clipling-gear { background:transparent; color:rgba(140,255,185,0.6); border:none; }
.clipling-gear:hover { color:rgba(180,255,210,1); }
button { background:rgba(0,20,10,0.6); color:rgba(180,255,210,0.95); border:1px solid rgba(100,255,155,0.3); border-radius:2px; padding:6px 14px; }
button.suggested-action { background:rgba(100,255,155,0.25); }

/* CRT power-on: thin flash line -> expand -> RGB split (red/cyan fringe on text),
   jitter and skew -> settle. Neighbouring keyframes repeat values so the shifts
   jump instead of sliding smoothly. */
@keyframes crt-on {
  0%   { opacity:1; transform: scale(0.5, 0.006); filter: brightness(4); }
  14%  { transform: scale(1.02, 0.01); filter: brightness(3.5); }
  26%  { transform: scale(1, 1.04) translateX(-8px) skewX(6deg); filter: brightness(1.8) hue-rotate(-30deg);
         text-shadow: -4px 0 rgba(255,40,90,0.9), 4px 0 rgba(0,255,240,0.9); }
  36%  { transform: scale(1, 1.04) translateX(-8px) skewX(6deg); filter: brightness(1.8) hue-rotate(-30deg);
         text-shadow: -4px 0 rgba(255,40,90,0.9), 4px 0 rgba(0,255,240,0.9); }
  37%  { transform: translateX(7px) skewX(-4deg); filter: brightness(1.3) hue-rotate(35deg);
         text-shadow: 5px 0 rgba(255,40,90,0.9), -5px 0 rgba(0,255,240,0.9); }
  50%  { transform: translateX(7px) skewX(-4deg); filter: brightness(1.3) hue-rotate(35deg);
         text-shadow: 5px 0 rgba(255,40,90,0.9), -5px 0 rgba(0,255,240,0.9); }
  51%  { transform: translateX(-3px); opacity:0.7;
         text-shadow: -2px 0 rgba(255,40,90,0.7), 2px 0 rgba(0,255,240,0.7); }
  64%  { transform: translateX(-3px); opacity:0.7;
         text-shadow: -2px 0 rgba(255,40,90,0.7), 2px 0 rgba(0,255,240,0.7); }
  65%  { transform: translateX(2px) skewX(1deg); opacity:1; filter: brightness(1.2);
         text-shadow: 1px 0 rgba(255,40,90,0.5), -1px 0 rgba(0,255,240,0.5); }
  80%  { transform: none; filter: none; text-shadow: 0 0 6px rgba(80,255,140,0.35); }
  100% { opacity:1; transform: none; filter: none; text-shadow: 0 0 6px rgba(80,255,140,0.35); }
}
/* Power-off: a twitch -> collapse into a bright line -> into a dot. */
@keyframes crt-off {
  0%   { opacity:1; transform: none; filter: none; }
  25%  { transform: translateX(5px) skewX(-4deg); filter: brightness(1.6);
         text-shadow: 4px 0 rgba(255,40,90,0.9), -4px 0 rgba(0,255,240,0.9); }
  55%  { opacity:1; transform: scale(1, 0.01); filter: brightness(4); }
  80%  { opacity:0.9; transform: scale(0.12, 0.006); filter: brightness(5); }
  100% { opacity:0; transform: scale(0, 0.004); }
}
/* "Blink" on Enter with several matches. */
@keyframes crt-glitch {
  0%   { transform: none; }
  20%  { transform: translateX(-6px) skewX(5deg); filter: hue-rotate(-30deg) brightness(1.5);
         text-shadow: -4px 0 rgba(255,40,90,0.9), 4px 0 rgba(0,255,240,0.9); }
  45%  { transform: translateX(5px) skewX(-3deg); filter: hue-rotate(30deg);
         text-shadow: 4px 0 rgba(255,40,90,0.9), -4px 0 rgba(0,255,240,0.9); }
  70%  { transform: translateX(-2px); filter: none;
         text-shadow: -1px 0 rgba(255,40,90,0.6), 1px 0 rgba(0,255,240,0.6); }
  100% { transform: none; filter: none; text-shadow: 0 0 6px rgba(80,255,140,0.35); }
}
window.clipling.anim-in  .clipling-root { animation: crt-on  230ms linear 1 both; }
window.clipling.anim-out .clipling-root { animation: crt-off 160ms linear 1 both; }
window.clipling.pulse    .clipling-root { animation: crt-glitch 200ms linear 1; }
)CSS";

static void ensure_defaults() {
    std::error_code ec; fs::create_directories(themes_dir(), ec);
    auto w = [](const fs::path& p, const char* data){ if (!fs::exists(p)) std::ofstream(p) << data; };
    w(themes_dir()/"dark.css",   DARK_CSS);
    w(themes_dir()/"light.css",  LIGHT_CSS);
    w(themes_dir()/"pipboy.css", PIPBOY_CSS);
    if (!fs::exists(config_file())) save_config(Config{});
}

static std::vector<std::string> list_themes() {
    std::vector<std::string> v;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(themes_dir(), ec))
        if (e.path().extension() == ".css") v.push_back(e.path().stem().string());
    std::sort(v.begin(), v.end());
    if (v.empty()) v = {"dark","light","pipboy"};
    return v;
}

static std::string read_theme_css(const std::string& name) {
    std::ifstream f(themes_dir() / (name + ".css"));
    if (f) return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (name == "light")  return LIGHT_CSS;
    if (name == "pipboy") return PIPBOY_CSS;
    return DARK_CSS;
}

// One provider per process: the theme is reloaded into it on every show.
static void apply_css(const std::string& theme) {
    static Glib::RefPtr<Gtk::CssProvider> provider;
    if (!provider) {
        provider = Gtk::CssProvider::create();
        if (auto display = Gdk::Display::get_default())
            // above USER (~/.config/gtk-4.0/gtk.css), otherwise the system theme overrides ours
            Gtk::StyleContext::add_provider_for_display(display, provider, GTK_STYLE_PROVIDER_PRIORITY_USER + 100);
    }
    provider->load_from_string(std::string(BASE_CSS) + "\n" + read_theme_css(theme));
}

// =====================================================================
//  Placement at the cursor (Hyprland)
// =====================================================================

static bool hypr_cursorpos(int& x, int& y) {
    std::string out;
    try { Glib::spawn_command_line_sync("hyprctl cursorpos", &out); } catch (...) { return false; }
    int cx, cy;
    if (std::sscanf(out.c_str(), "%d, %d", &cx, &cy) == 2 ||
        std::sscanf(out.c_str(), "%d %d",  &cx, &cy) == 2) { x = cx; y = cy; return true; }
    return false;
}


// =====================================================================
//  Picker window
// =====================================================================

class Picker : public Gtk::ApplicationWindow {
public:
    // The window is created once per process and then only hidden/shown.
    Picker() {
        set_title("clipling");
        set_decorated(false);
        add_css_class("clipling");
        add_css_class("pre");   // invisible until the first frame, otherwise the animation plays unseen

        // Transparent overlay layer over the whole monitor, the panel is drawn at the cursor.
        // A click outside the panel still lands on us -> close. Focus loss can't do this:
        // with follow_mouse, focus moves away on a mere hover.
        gtk_layer_init_for_window(Gtk::Window::gobj());
        gtk_layer_set_namespace(Gtk::Window::gobj(), "clipling");
        gtk_layer_set_layer(Gtk::Window::gobj(), GTK_LAYER_SHELL_LAYER_OVERLAY);
        gtk_layer_set_keyboard_mode(Gtk::Window::gobj(), GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
        gtk_layer_set_exclusive_zone(Gtk::Window::gobj(), -1);
        for (auto edge : { GTK_LAYER_SHELL_EDGE_LEFT, GTK_LAYER_SHELL_EDGE_RIGHT,
                           GTK_LAYER_SHELL_EDGE_TOP,  GTK_LAYER_SHELL_EDGE_BOTTOM })
            gtk_layer_set_anchor(Gtk::Window::gobj(), edge, true);

        root_.add_css_class("clipling-root");
        root_.set_halign(Gtk::Align::START);
        root_.set_valign(Gtk::Align::START);
        set_child(root_);

        // ---- header: search + gear ----
        auto header = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
        header->add_css_class("clipling-header");
        search_.set_placeholder_text("search…");
        search_.set_hexpand(true);
        search_.add_css_class("clipling-search");
        header->append(search_);
        gear_.set_label("⚙");               // ⚙
        gear_.add_css_class("clipling-gear");
        gear_.set_tooltip_text("Settings");
        header->append(gear_);
        root_.append(*header);

        // ---- split ----
        paned_.set_vexpand(true); paned_.set_wide_handle(true);
        root_.append(paned_);

        listbox_.add_css_class("clipling-text");
        listbox_.set_activate_on_single_click(true);
        left_.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
        left_.set_child(listbox_);

        flow_.add_css_class("clipling-images");
        flow_.set_selection_mode(Gtk::SelectionMode::SINGLE);
        flow_.set_activate_on_single_click(true);
        flow_.set_valign(Gtk::Align::START);
        flow_.set_min_children_per_line(2);
        flow_.set_homogeneous(true);
        right_.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
        right_.set_child(flow_);

        paned_.set_start_child(left_);
        paned_.set_end_child(right_);

        // filters
        listbox_.set_filter_func([this](Gtk::ListBoxRow* row) {
            int i = row->get_index();
            if (i < 0 || (size_t)i >= text_rows_.size()) return true;
            return query_.empty() || text_rows_[i].second.find(query_) != std::string::npos;
        });
        flow_.set_filter_func([this](Gtk::FlowBoxChild*) { return query_.empty(); });

        // activation -> copy and close
        listbox_.signal_row_activated().connect([this](Gtk::ListBoxRow* row){
            int i = row->get_index();
            if (i >= 0 && (size_t)i < text_entries_.size()) copy_and_close(text_entries_[i]);
        });
        flow_.signal_child_activated().connect([this](Gtk::FlowBoxChild* ch){
            int i = ch->get_index();
            if (i >= 0 && (size_t)i < image_entries_.size()) copy_and_close(image_entries_[i]);
        });

        // live filter with debounce
        search_.signal_search_changed().connect([this]{
            query_ = fold_text(search_.get_text());
            if (filter_timer_.connected()) filter_timer_.disconnect();
            filter_timer_ = Glib::signal_timeout().connect([this]{
                listbox_.invalidate_filter(); flow_.invalidate_filter(); return false;
            }, cfg_.debounce_ms);
        });

        // Enter in the search field
        search_.signal_activate().connect([this]{ on_enter(); });

        // Down from search -> into the list; Esc -> close
        auto sk = Gtk::EventControllerKey::create();
        sk->signal_key_pressed().connect([this](guint kv, guint, Gdk::ModifierType) -> bool {
            if (kv == GDK_KEY_Down) { focus_first_visible(); return true; }
            if (kv == GDK_KEY_Escape) { close_animated(); return true; }
            return false;
        }, false);
        search_.add_controller(sk);

        auto wk = Gtk::EventControllerKey::create();
        wk->signal_key_pressed().connect([this](guint kv, guint, Gdk::ModifierType) -> bool {
            if (kv == GDK_KEY_Escape) { close_animated(); return true; }
            return false;
        }, false);
        add_controller(wk);

        // the overlay would cover the settings window — make way for it
        gear_.signal_clicked().connect([this]{ close_animated(); signal_settings_.emit(); });

        // show with the animation once mapped
        // (an animation started before map plays unseen — that's why it "wasn't there")
        signal_map().connect([this]{
            Glib::signal_idle().connect_once([this]{ reveal(); });
        });

        // click outside the panel -> close
        auto outside = Gtk::GestureClick::create();
        outside->set_button(0);
        outside->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
        outside->signal_pressed().connect([this](int, double x, double y){
            graphene_rect_t r;
            if (!gtk_widget_compute_bounds(root_.Gtk::Widget::gobj(), Gtk::Widget::gobj(), &r)) return;
            if (x < r.origin.x || y < r.origin.y ||
                x > r.origin.x + r.size.width || y > r.origin.y + r.size.height) close_animated();
        });
        add_controller(outside);
    }

    sigc::signal<void()>& signal_settings() { return signal_settings_; }

    // Load config, theme and history ahead of time without showing the window.
    void prepare() {
        cfg_ = load_config();
        apply_css(cfg_.theme);
        root_.set_size_request(cfg_.width, cfg_.height);
        paned_.set_position(cfg_.split > 0 ? cfg_.split : cfg_.width * 58 / 100);
        flow_.set_max_children_per_line(cfg_.image_cols);
        if (cfg_.thumb != thumb_size_) { thumb_size_ = cfg_.thumb; thumbs_.clear(); stamp_.clear(); }
        auto stamp = history_stamp();
        if (stamp.empty() || stamp != stamp_) { stamp_ = stamp; populate(); }
    }

    // Calling again while the window is open closes it — like rofi.
    void toggle() {
        if (get_visible() && !closing_) { close_animated(); return; }
        open();
    }

private:
    void open() {
        if (hide_timer_.connected()) hide_timer_.disconnect();
        prepare();
        closing_ = false;
        remove_css_class("anim-in"); remove_css_class("anim-out"); remove_css_class("pulse");
        add_css_class("pre");

        search_.set_text("");
        query_.clear();
        listbox_.invalidate_filter(); flow_.invalidate_filter();
        listbox_.unselect_all(); flow_.unselect_all();
        left_.get_vadjustment()->set_value(0);
        right_.get_vadjustment()->set_value(0);

        place_near_cursor();
        if (get_visible()) reveal();   // was closing — restart the animation in place
        else present();
        search_.grab_focus();
    }

    void populate() {
        thumb_gen_++;                  // the old background decoder will give up
        listbox_.remove_all();
        flow_.remove_all();
        text_entries_.clear(); image_entries_.clear(); text_rows_.clear(); pics_.clear();

        for (auto& e : load_entries()) {
            if (e.mime.empty()) text_entries_.push_back(std::move(e));
            else                image_entries_.push_back(std::move(e));
        }
        for (auto& e : text_entries_) {
            auto row = Gtk::make_managed<Gtk::ListBoxRow>();
            row->add_css_class("clipling-row");
            auto lbl = Gtk::make_managed<Gtk::Label>(e.text);
            lbl->set_xalign(0.0f);
            lbl->set_ellipsize(Pango::EllipsizeMode::END);
            row->set_child(*lbl);
            listbox_.append(*row);
            text_rows_.push_back({ row, std::move(e.hay) });
        }

        const int maxw = thumb_size_, maxh = thumb_size_ * 3 / 4;
        std::vector<int64_t> pending;
        std::map<int64_t, Glib::RefPtr<Gdk::Texture>> kept;
        for (auto& e : image_entries_) {
            auto child = Gtk::make_managed<Gtk::FlowBoxChild>();
            child->add_css_class("clipling-tile");
            auto pic = Gtk::make_managed<Gtk::Picture>();
            pic->set_size_request(maxw, maxh);
            pic->set_content_fit(Gtk::ContentFit::CONTAIN);
            child->set_child(*pic);
            flow_.append(*child);
            pics_[e.id] = pic;
            if (auto it = thumbs_.find(e.id); it != thumbs_.end()) {
                pic->set_paintable(it->second);
                kept.insert(*it);
            } else pending.push_back(e.id);
        }
        thumbs_ = std::move(kept);     // entries removed from history drop out of the cache
        if (!pending.empty()) decode_thumbs(std::move(pending), maxw, maxh);
    }

    // Thumbnails are decoded in the background: the window doesn't wait for
    // images, tiles fill in as they become ready.
    void decode_thumbs(std::vector<int64_t> ids, int maxw, int maxh) {
        const unsigned gen = thumb_gen_;
        std::thread([this, ids = std::move(ids), maxw, maxh, gen]{
            sqlite3* db = open_db();
            if (!db) return;
            for (auto id : ids) {
                if (gen != thumb_gen_) break;
                auto data = load_content(db, id);
                Glib::RefPtr<Gdk::Texture> tex;
                try {
                    auto stream = Gio::MemoryInputStream::create();
                    stream->add_bytes(Glib::Bytes::create(data.data(), data.size()));
                    auto pixbuf = Gdk::Pixbuf::create_from_stream(stream);
                    if (!pixbuf) continue;
                    int w = pixbuf->get_width(), h = pixbuf->get_height();
                    if (w > 0 && h > 0) {
                        double r = std::min({ (double)maxw / w, (double)maxh / h, 1.0 });
                        pixbuf = pixbuf->scale_simple(std::max(1,(int)(w*r)), std::max(1,(int)(h*r)), Gdk::InterpType::BILINEAR);
                    }
                    tex = Gdk::Texture::create_for_pixbuf(pixbuf);
                } catch (const Glib::Error&) { continue; }
                Glib::MainContext::get_default()->invoke([this, id, tex, gen]{
                    if (gen != thumb_gen_) return false;
                    thumbs_[id] = tex;
                    if (auto it = pics_.find(id); it != pics_.end()) it->second->set_paintable(tex);
                    return false;
                });
            }
            sqlite3_close(db);
        }).detach();
    }

    void on_enter() {
        std::vector<int> vis;
        for (size_t i = 0; i < text_rows_.size(); ++i)
            if (query_.empty() || text_rows_[i].second.find(query_) != std::string::npos) vis.push_back((int)i);
        size_t total = vis.size() + (query_.empty() ? image_entries_.size() : 0);
        if (query_.empty() && vis.empty() && image_entries_.size() == 1) { copy_and_close(image_entries_[0]); return; }
        if (total == 1 && vis.size() == 1) { copy_and_close(text_entries_[vis[0]]); return; }
        // several -> blink + move the selection to the first one
        pulse();
        if (!vis.empty()) { if (auto* row = listbox_.get_row_at_index(vis.front())) { listbox_.select_row(*row); row->grab_focus(); } }
    }

    void focus_first_visible() {
        for (size_t i = 0; i < text_rows_.size(); ++i)
            if (query_.empty() || text_rows_[i].second.find(query_) != std::string::npos) {
                if (auto* row = listbox_.get_row_at_index((int)i)) { listbox_.select_row(*row); row->grab_focus(); }
                return;
            }
    }

    void reveal() {
        if (closing_) return;
        // anim-in stays until close: the last frame (fill both) equals the normal look
        if (cfg_.animations) add_css_class("anim-in");
        remove_css_class("pre");
    }

    void pulse() {
        if (!cfg_.animations) return;
        add_css_class("pulse");
        Glib::signal_timeout().connect_once([this]{ remove_css_class("pulse"); }, 260);
    }

    void copy_and_close(const Entry& e) { copy_entry(e, cfg_); close_animated(); }

    void close_animated() {
        if (closing_) return;
        closing_ = true;
        if (cfg_.animations) {
            remove_css_class("anim-in"); add_css_class("anim-out");
            hide_timer_ = Glib::signal_timeout().connect([this]{ hide(); return false; }, 170);
        } else hide();
    }

    // The layer covers the monitor under the cursor; the panel is placed by margins inside it.
    // Without hyprctl the cursor is unknown -> the panel is centered on the first monitor.
    void place_near_cursor() {
        int cx = 0, cy = 0;
        bool have_cursor = hypr_cursorpos(cx, cy);
        const int W = cfg_.width, H = cfg_.height;
        Gdk::Rectangle mon; bool found = false;
        Glib::RefPtr<Gdk::Monitor> mon_obj;
        if (auto disp = Gdk::Display::get_default()) {
            auto mons = disp->get_monitors();
            for (guint i = 0; i < mons->get_n_items(); ++i) {
                auto m = std::dynamic_pointer_cast<Gdk::Monitor>(mons->get_object(i));
                if (!m) continue;
                Gdk::Rectangle g; m->get_geometry(g);
                if (i == 0) { mon = g; mon_obj = m; }
                if (have_cursor && cx >= g.get_x() && cx < g.get_x()+g.get_width() && cy >= g.get_y() && cy < g.get_y()+g.get_height()) {
                    mon = g; mon_obj = m; found = true; break;
                }
            }
        }
        if (mon_obj) gtk_layer_set_monitor(Gtk::Window::gobj(), mon_obj->gobj());
        if (mon.get_width() <= 0) return;
        const int M = 8, GAP = cfg_.gap;
        int left = mon.get_x()+M, top = mon.get_y()+M;
        int right = mon.get_x()+mon.get_width()-M, bottom = mon.get_y()+mon.get_height()-M;
        int x = cx + GAP, y = cy + GAP;                 // default: below and to the right of the cursor
        if (x + W > right)  x = cx - GAP - W;           // doesn't fit on the right -> left
        if (y + H > bottom) y = cy - GAP - H;           // doesn't fit below -> above
        x = std::clamp(x, left, std::max(left, right - W));
        y = std::clamp(y, top,  std::max(top,  bottom - H));
        if (W > mon.get_width()  - 2*M || !found) x = mon.get_x() + (mon.get_width()  - W)/2;
        if (H > mon.get_height() - 2*M || !found) y = mon.get_y() + (mon.get_height() - H)/2;
        root_.set_margin_start(x - mon.get_x());
        root_.set_margin_top(y - mon.get_y());
    }

    Config cfg_;
    Gtk::Box         root_{Gtk::Orientation::VERTICAL, 0};
    Gtk::Paned       paned_{Gtk::Orientation::HORIZONTAL};
    Gtk::ScrolledWindow left_, right_;
    Gtk::SearchEntry search_;
    Gtk::Button      gear_;
    Gtk::ListBox     listbox_;
    Gtk::FlowBox     flow_;
    std::vector<Entry> text_entries_, image_entries_;
    std::vector<std::pair<Gtk::ListBoxRow*, std::string>> text_rows_;
    std::map<int64_t, Gtk::Picture*> pics_;
    std::map<int64_t, Glib::RefPtr<Gdk::Texture>> thumbs_;
    std::atomic<unsigned> thumb_gen_{0};
    int thumb_size_ = 0;
    std::string stamp_;
    std::string query_;
    sigc::connection filter_timer_, hide_timer_;
    sigc::signal<void()> signal_settings_;
    bool closing_ = false;
};

// =====================================================================
//  Settings window
// =====================================================================

class Settings : public Gtk::ApplicationWindow {
public:
    Settings() {
        cfg_ = load_config();
        apply_css(cfg_.theme);
        set_title("clipling · settings");
        set_default_size(420, 560);
        add_css_class("clipling");

        auto root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
        root->add_css_class("clipling-root");
        root->add_css_class("clipling-settings");
        set_child(*root);

        auto title = Gtk::make_managed<Gtk::Label>("clipling settings");
        title->add_css_class("title"); title->set_xalign(0.0f); title->set_margin_bottom(14);
        root->append(*title);

        auto grid = Gtk::make_managed<Gtk::Grid>();
        grid->set_row_spacing(12); grid->set_column_spacing(14);
        root->append(*grid);
        int r = 0;

        // theme
        themes_ = list_themes();
        auto strlist = Gtk::StringList::create({});
        int sel = 0;
        for (size_t i = 0; i < themes_.size(); ++i) { strlist->append(themes_[i]); if (themes_[i] == cfg_.theme) sel = (int)i; }
        theme_dd_.set_model(strlist);
        theme_dd_.set_selected(sel);
        add_row(*grid, r++, "Theme", theme_dd_);

        setup_spin(w_spin_, 320, 2200, 10, cfg_.width);      add_row(*grid, r++, "Window width, px", w_spin_);
        setup_spin(h_spin_, 240, 1600, 10, cfg_.height);     add_row(*grid, r++, "Window height, px", h_spin_);
        setup_spin(split_spin_, 120, 2000, 10, cfg_.split > 0 ? cfg_.split : cfg_.width * 58 / 100);
        add_row(*grid, r++, "Text panel width, px", split_spin_);
        setup_spin(gap_spin_, 0, 200, 2, cfg_.gap);          add_row(*grid, r++, "Gap from cursor, px", gap_spin_);
        setup_spin(thumb_spin_, 48, 320, 8, cfg_.thumb);     add_row(*grid, r++, "Thumbnail size, px", thumb_spin_);
        setup_spin(cols_spin_, 1, 16, 1, cfg_.image_cols);   add_row(*grid, r++, "Image columns", cols_spin_);
        setup_spin(deb_spin_, 0, 1000, 10, cfg_.debounce_ms);add_row(*grid, r++, "Filter delay, ms", deb_spin_);

        anim_sw_.set_active(cfg_.animations);     anim_sw_.set_halign(Gtk::Align::START);  add_row(*grid, r++, "Animations", anim_sw_);
        prim_sw_.set_active(cfg_.copy_primary);   prim_sw_.set_halign(Gtk::Align::START);  add_row(*grid, r++, "Copy to primary", prim_sw_);
        clip_sw_.set_active(cfg_.copy_clipboard); clip_sw_.set_halign(Gtk::Align::START);  add_row(*grid, r++, "Copy to clipboard", clip_sw_);

        auto buttons = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
        buttons->set_halign(Gtk::Align::END); buttons->set_margin_top(18);
        auto cancel = Gtk::make_managed<Gtk::Button>("Close");
        auto save   = Gtk::make_managed<Gtk::Button>("Save");
        save->add_css_class("suggested-action");
        cancel->signal_clicked().connect([this]{ hide(); });
        save->signal_clicked().connect([this]{ on_save(); });
        buttons->append(*cancel); buttons->append(*save);
        root->append(*buttons);
    }

private:
    static void setup_spin(Gtk::SpinButton& s, int lo, int hi, int step, int val) {
        s.set_range(lo, hi); s.set_increments(step, step*5); s.set_value(val); s.set_halign(Gtk::Align::START);
    }
    static void add_row(Gtk::Grid& g, int r, const char* label, Gtk::Widget& ctrl) {
        auto l = Gtk::make_managed<Gtk::Label>(label);
        l->set_xalign(0.0f); l->set_hexpand(true);
        g.attach(*l, 0, r); g.attach(ctrl, 1, r);
    }
    void on_save() {
        guint idx = theme_dd_.get_selected();
        if (idx != GTK_INVALID_LIST_POSITION && idx < themes_.size()) cfg_.theme = themes_[idx];
        cfg_.width = w_spin_.get_value_as_int();
        cfg_.height = h_spin_.get_value_as_int();
        cfg_.split = split_spin_.get_value_as_int();
        cfg_.gap = gap_spin_.get_value_as_int();
        cfg_.thumb = thumb_spin_.get_value_as_int();
        cfg_.image_cols = cols_spin_.get_value_as_int();
        cfg_.debounce_ms = deb_spin_.get_value_as_int();
        cfg_.animations = anim_sw_.get_active();
        cfg_.copy_primary = prim_sw_.get_active();
        cfg_.copy_clipboard = clip_sw_.get_active();
        save_config(cfg_);
        apply_css(cfg_.theme);   // apply the theme to this window right away
        hide();
    }

    Config cfg_;
    std::vector<std::string> themes_;
    Gtk::DropDown  theme_dd_;
    Gtk::SpinButton w_spin_, h_spin_, split_spin_, gap_spin_, thumb_spin_, cols_spin_, deb_spin_;
    Gtk::Switch     anim_sw_, prim_sw_, clip_sw_;
};

// =====================================================================

static void open_settings(const Glib::RefPtr<Gtk::Application>& app) {
    for (auto* w : app->get_windows())
        if (auto* st = dynamic_cast<Settings*>(w)) { st->present(); return; }
    auto* st = new Settings();
    app->add_window(*st);
    st->signal_hide().connect([st]{ Glib::signal_idle().connect_once([st]{ delete st; }); });
    st->present();
}

// clipling            — show/hide the picker
// clipling --settings — settings window
// clipling --daemon   — just start the process (for autostart), no window
// clipling --quit     — stop the resident process (e.g. after a rebuild)
int main(int argc, char* argv[]) {
    ensure_defaults();

    // A valid id is needed for single-instance mode: later launches only forward
    // their arguments to the running process. It is also the settings window class in Hyprland.
    Glib::set_prgname("clipling");
    auto app = Gtk::Application::create("io.github.serlk.clipling",
                                        Gio::Application::Flags::HANDLES_COMMAND_LINE);
    Picker* picker = nullptr;

    app->signal_startup().connect([&]{
        app->hold();                       // stay alive without visible windows
        picker = new Picker();
        app->add_window(*picker);
        picker->signal_settings().connect([&]{ open_settings(app); });
        picker->prepare();                 // history and thumbnails ahead of time
    });

    app->signal_command_line().connect([&](const Glib::RefPtr<Gio::ApplicationCommandLine>& cl) -> int {
        int n = 0;
        char** args = cl->get_arguments(n);
        bool settings = false, daemon = false, quit = false;
        for (int i = 1; i < n; ++i) {
            std::string a = args[i];
            if (a == "--settings") settings = true;
            else if (a == "--daemon") daemon = true;
            else if (a == "--quit") quit = true;
        }
        g_strfreev(args);
        if (quit) app->quit();
        else if (settings) open_settings(app);
        else if (!daemon) picker->toggle();
        return 0;
    }, false);

    return app->run(argc, argv);
}
