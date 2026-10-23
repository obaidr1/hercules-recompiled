// Hercules launcher, compiled into the game executable.
//
// Runs before the game (psx_game_launcher.h): a 4:3 stage in the style of the
// 1997 title menus (main menu, level select, options, save states, five
// languages), drawn with SDL3 from art decoded off the player's own disc
// (hl_data). "Play" writes settings.toml and mods/state.toml like the Python
// launcher did, then runs this same executable with --no-launcher and waits.
#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "hl_data.h"
#include "hl_strings.h"
#include "psx_game_launcher.h"
#include "psx_game_menu.h"

// The framework's keyboard bindings (runtime/include/psx_keybinds.h; that
// header pulls in an SDL2 compatibility layer, so only what is used here,
// with the button order of enum PsxKeybindButton).
extern "C" {
const char* psx_keybinds_button_name(int button);
SDL_Scancode psx_keybinds_get_button(int player, int button);
void psx_keybinds_set_button(int player, int button, SDL_Scancode sc);
void psx_keybinds_set_button_alt(int player, int button, SDL_Scancode sc);
void psx_keybinds_reset_player(int player);
void psx_keybinds_save(void);
}
enum {
    PSX_KB_UP = 0, PSX_KB_DOWN, PSX_KB_LEFT, PSX_KB_RIGHT, PSX_KB_CROSS, PSX_KB_CIRCLE, PSX_KB_SQUARE,
    PSX_KB_TRIANGLE, PSX_KB_L1, PSX_KB_R1, PSX_KB_L2, PSX_KB_R2, PSX_KB_L3, PSX_KB_R3, PSX_KB_START,
    PSX_KB_SELECT, PSX_KB_LS_UP, PSX_KB_LS_DOWN, PSX_KB_LS_LEFT, PSX_KB_LS_RIGHT
};

#ifdef _WIN32
extern "C" __declspec(dllimport) unsigned long __stdcall GetCurrentProcessId(void);
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace hl {
namespace {

constexpr int W = 320, H = 256, SC = 4;           // stage in PAL pixels, drawn at 4x
// The five PAL releases (hercules/docs/pal_versions.md). Each disc holds one
// language and needs its own build: the launcher's language picks the
// release, the player supplies that release's disc. The German build sits in
// the game folder, the others in lang/<id>/ (own game.toml, saves, caches;
// settings, keys, discs and HD packs stay in the game folder).
struct Release { const char* lang; const char* id; const char* boot; uint32_t dist_table; };
const Release kReleases[] = {
    {"de", "SCES-00893", "SCES_008.93", 0x8006AB3B}, {"en", "SCES-00891", "SCES_008.91", 0x8006AB4F},
    {"fr", "SCES-00892", "SCES_008.92", 0x8006AB3F}, {"it", "SCES-00894", "SCES_008.94", 0x8006AB3B},
    {"es", "SCES-00895", "SCES_008.95", 0x8006AB03},
};
#ifndef HERCULES_GAME_ID
#define HERCULES_GAME_ID "SCES-00893"
#endif
const char* const OWN_ID = HERCULES_GAME_ID;     // the release this exe was built for
const Release& release(const std::string& id) {
    for (const auto& r : kReleases) if (id == r.id) return r;
    return kReleases[0];
}
const Release* release_for_lang(const std::string& lang) {
    for (const auto& r : kReleases) if (lang == r.lang) return &r;
    return nullptr;
}

// --------------------------------------------------------------- utilities
std::u32string utf32(const std::string& s) {
    std::u32string out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = s[i];
        char32_t cp;
        int n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1F; n = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0F; n = 3; }
        else { cp = c & 0x07; n = 4; }
        for (int k = 1; k < n && i + k < s.size(); ++k) cp = (cp << 6) | (s[i + k] & 0x3F);
        out.push_back(cp);
        i += n;
    }
    return out;
}

char32_t upper(char32_t c) {
    if (c >= U'a' && c <= U'z') return c - 32;
    if ((c >= 0xE0 && c <= 0xFE && c != 0xF7)) return c - 32;     // Latin-1 lower -> upper
    return c;
}

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::string read_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), {});
}

bool write_atomic(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = p.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << text;
    }
    fs::rename(tmp, p, ec);
    return !ec;
}

// Tiny TOML reader for the flat keys we need: [section] key = value.
std::map<std::string, std::string> toml_flat(const std::string& text) {
    std::map<std::string, std::string> out;
    std::string section;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '[') { section = trim(line.substr(1, line.find(']') - 1)); continue; }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string v = trim(line.substr(eq + 1));
        if (!v.empty() && v[0] == '"') v = v.substr(1, v.find('"', 1) - 1);
        else v = trim(v.substr(0, v.find('#')));
        out[(section.empty() ? "" : section + ".") + trim(line.substr(0, eq))] = v;
    }
    return out;
}

std::string toml_str(const std::string& s) {
    std::string o = "\"";
    for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; }
    return o + "\"";
}

// ---------------------------------------------------------------- settings
// Same keys and defaults as launcher/hercules_launcher.py DEFAULTS.
struct Setting { const char* key; const char* kind; const char* def; };
const Setting kSettings[] = {
    {"internal_resolution", "s", "1080p"}, {"window_width", "i", "0"}, {"fullscreen", "i", "0"},
    {"aspect_ratio", "s", "4:3"}, {"vsync", "s", "on"}, {"texture_filtering", "s", "nearest"},
    {"antialiasing", "b", "false"}, {"crt_filter", "s", "raw"}, {"scanlines", "b", "false"},
    {"scanline_strength", "f", "0.35"}, {"perspective_texturing", "b", "false"},
    {"geometry_correction", "b", "false"}, {"auto_skip_fmv", "b", "false"}, {"rewind", "b", "true"},
    {"frequency", "i", "48000"}, {"spu_hq", "b", "true"}, {"hd_sprites", "b", "true"},
    {"hd_levels", "b", "true"}, {"hd_hud", "b", "true"}, {"start_level", "s", "off"},
    {"crt_sim", "b", "false"}, {"crt_mask", "f", "0.5"}, {"crt_bloom", "f", "0.35"},
    {"skip_intro", "b", "true"}, {"skip_memcard", "b", "true"}, {"language", "s", ""}, {"fast_cd", "b", "true"},
    {"updates", "b", "true"}, {"frame_blend", "s", "off"}, {"ntsc_speed", "b", "true"},
};
const char* const kVideoKeys[] = {"internal_resolution", "window_width", "fullscreen", "aspect_ratio", "vsync",
                                  "texture_filtering", "antialiasing", "crt_filter", "scanlines",
                                  "scanline_strength", "perspective_texturing", "geometry_correction",
                                  "rewind"};
const std::map<std::string, std::map<std::string, std::string>> kPresets = {
    {"original", {{"internal_resolution", "native"}, {"texture_filtering", "nearest"}, {"antialiasing", "false"},
                  {"crt_filter", "crt"}, {"scanlines", "true"}, {"crt_sim", "true"}, {"scanline_strength", "0.35"},
                  {"hd_sprites", "false"}, {"hd_levels", "false"}, {"hd_hud", "false"},
                  {"perspective_texturing", "false"}}},
    {"hd", {{"internal_resolution", "1080p"}, {"texture_filtering", "nearest"}, {"antialiasing", "false"},
            {"crt_filter", "raw"}, {"scanlines", "false"}, {"crt_sim", "false"},
            {"hd_sprites", "true"}, {"hd_levels", "true"}, {"hd_hud", "true"}}},
};

const Setting* setting(const std::string& key) {
    for (const auto& s : kSettings) if (key == s.key) return &s;
    return nullptr;
}

std::string toml_value(const std::string& key, const std::string& v) {
    const Setting* s = setting(key);
    if (!s || s->kind[0] == 's') return toml_str(v);
    return v;
}

struct Store {
    fs::path path;
    std::map<std::string, std::string> kv;           // settings + slot_name.N / slot_mtime.N
    void load() {
        for (const auto& s : kSettings) kv[s.key] = s.def;
        std::istringstream in(read_text(path));
        for (std::string line; std::getline(in, line);) {
            const size_t eq = line.find('=');
            if (eq != std::string::npos) kv[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
        }
    }
    void save() const {
        std::string t = "# Hercules launcher settings (written by the game's launcher)\n";
        for (const auto& [k, v] : kv) t += k + " = " + v + "\n";
        write_atomic(path, t);
    }
    std::string get(const std::string& k) const { auto it = kv.find(k); return it == kv.end() ? "" : it->second; }
    bool b(const std::string& k) const { return get(k) == "true"; }
};

// ------------------------------------------------------------ the context
// exe_dir is the game folder (launcher.ini, discs, HD packs, keys); id the
// release in use and game_dir/exe its build (game.toml, saves, settings.toml).
struct Ctx {
    fs::path exe, exe_dir, game_toml, disc;
    std::string id = OWN_ID;
    fs::path game_dir, launcher_exe;
    Store store;
    std::map<std::string, std::string> game;         // game.toml flattened
    std::string preset() const {
        for (const auto& [name, values] : kPresets) {
            bool all = true;
            for (const auto& [k, v] : values) all &= store.get(k) == v;
            if (all) return name;
        }
        return "custom";
    }
};

fs::path find_up(const fs::path& start, const fs::path& rel) {
    for (fs::path d = start;; d = d.parent_path()) {
        if (fs::exists(d / rel)) return d / rel;
        if (d == d.parent_path()) break;
    }
    return {};
}

// ---------------------------------------------------------------- packs
fs::path pack_base(const Ctx& c) { return c.exe_dir / "mods" / "texture-packs" / c.id; }

// A finished pack has its config.yaml and images in at least one part. The
// framework creates an empty <pack>/{replacements,dumps} on every start that
// has no pack configured, so "replacements/ exists" alone means nothing.
bool pack_complete(const fs::path& base) {
    if (!fs::exists(base / "config.yaml")) return false;
    for (const char* p : {"sprites", "levels", "hud"}) {
        std::error_code ec;
        if (fs::is_directory(base / "replacements" / p, ec) &&
            !fs::is_empty(base / "replacements" / p, ec)) return true;
    }
    return false;
}

std::vector<std::string> chosen_parts(const Ctx& c) {
    std::vector<std::string> p;
    if (c.store.b("hd_sprites")) p.push_back("sprites");
    if (c.store.b("hd_levels")) p.push_back("levels");
    if (c.store.b("hd_hud")) p.push_back("hud");
    return p;
}

void link_tree(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    fs::create_directories(dst, ec);
    for (auto it = fs::recursive_directory_iterator(src, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const fs::path t = dst / fs::relative(it->path(), src, ec);
        if (it->is_directory(ec)) { fs::create_directories(t, ec); continue; }
        fs::create_hard_link(it->path(), t, ec);
        if (ec) { ec.clear(); fs::copy_file(it->path(), t, fs::copy_options::overwrite_existing, ec); }
    }
}

// The pack folder for the chosen HD parts; a subset is a hard-linked copy of
// the full pack (as launcher/hercules_launcher.py build_variant does).
fs::path pack_for(const Ctx& c) {
    const fs::path base = pack_base(c);
    const auto parts = chosen_parts(c);
    if (parts.empty() || !pack_complete(base)) return {};
    std::vector<std::string> present;
    for (const char* p : {"sprites", "levels", "hud"})
        if (fs::is_directory(base / "replacements" / p)) present.push_back(p);
    bool all = true;
    for (const auto& p : present) all &= std::find(parts.begin(), parts.end(), p) != parts.end();
    if (all) return base;
    std::string name = c.id + ".launcher-";
    for (size_t i = 0; i < parts.size(); ++i) name += (i ? "+" : "") + parts[i];
    const fs::path variant = base.parent_path() / name;
    std::string stamp;
    for (const auto& p : parts) {
        std::error_code ec;
        const auto t = fs::last_write_time(base / "replacements" / p, ec);
        stamp += p + ":" + std::to_string(ec ? 0 : t.time_since_epoch().count()) + ";";
    }
    if (read_text(variant / "launcher-source.txt") != stamp) {
        std::error_code ec;
        const fs::path tmp = variant.string() + ".tmp";
        fs::remove_all(tmp, ec);
        fs::create_directories(tmp / "replacements", ec);
        fs::create_directories(tmp / "dumps", ec);
        fs::copy_file(base / "config.yaml", tmp / "config.yaml", fs::copy_options::overwrite_existing, ec);
        for (const auto& p : parts)
            if (fs::is_directory(base / "replacements" / p)) link_tree(base / "replacements" / p, tmp / "replacements" / p);
        write_atomic(tmp / "launcher-source.txt", stamp);
        fs::remove_all(variant, ec);
        fs::rename(tmp, variant, ec);
    }
    return variant;
}

// ------------------------------------------------------------- HD build
// The HD pack is made on the player's PC from their disc: hd/hd_auto.py in
// the Python that ships for the overlay toolchain fetches what it needs and
// runs the decomp's hd_pack_build.py. It starts by itself after the first
// game, runs at low priority while the launcher (or the game it started) is
// open, stops with the launcher and continues on the next start.
struct HdStatus { std::string state, phase, error; double fraction = 0; long eta = -1; };

fs::path hd_data(const Ctx& c) {
    return c.exe_dir / (c.id == "SCES-00893" ? std::string("hd-build") : "hd-build-" + c.id);
}
bool hd_pack_ready(const Ctx& c) { return pack_complete(pack_base(c)); }
fs::path hd_python(const Ctx& c) {
#ifdef _WIN32
    return c.exe_dir / "overlay_toolchain" / "python" / "pythonw.exe";     // no console window
#else
    return c.exe_dir / "overlay_toolchain" / "python" / "bin" / "python3";
#endif
}

// hd-build/progress.json: {"phase": "build", "fraction": 0.35, "eta": 5195, "state": "running", "error": null}
HdStatus hd_read(const Ctx& c) {
    HdStatus st;
    const std::string j = read_text(hd_data(c) / "progress.json");
    auto field = [&](const char* key) -> std::string {
        const size_t k = j.find(std::string("\"") + key + "\":");
        if (k == std::string::npos) return "";
        size_t v = j.find_first_not_of(' ', k + std::strlen(key) + 3);
        if (v == std::string::npos) return "";
        if (j[v] == '"') { const size_t e = j.find('"', v + 1); return e == std::string::npos ? "" : j.substr(v + 1, e - v - 1); }
        const size_t e = j.find_first_of(",}", v);
        const std::string raw = trim(j.substr(v, e - v));
        return raw == "null" ? "" : raw;
    };
    st.state = field("state");
    st.phase = field("phase");
    st.error = field("error");
    st.fraction = std::atof(field("fraction").c_str());
    const std::string eta = field("eta");
    st.eta = eta.empty() ? -1 : std::atol(eta.c_str());
    return st;
}

// --------------------------------------------------------------- updates
// update/updater.py (in the shipped Python) asks GitHub for the latest
// release, downloads and checks it, and swaps the program files after the
// launcher has quit; the player's files are never in the zip.
fs::path upd_dir(const Ctx& c) { return c.exe_dir / "update"; }

// A JSON string or literal by key (one level; enough for the updater's files).
std::string json_field(const std::string& j, const std::string& key) {
    size_t k = j.find("\"" + key + "\":");
    if (k == std::string::npos) return "";
    size_t v = j.find_first_not_of(" ", k + key.size() + 3);
    if (v == std::string::npos) return "";
    if (j[v] != '"') {
        const size_t e = j.find_first_of(",}", v);
        return trim(j.substr(v, e - v));
    }
    std::string out;
    for (size_t i = v + 1; i < j.size(); ++i) {
        const char ch = j[i];
        if (ch == '"') break;
        if (ch != '\\' || i + 1 >= j.size()) { out += ch; continue; }
        const char e = j[++i];
        if (e == 'n') out += '\n';
        else if (e == 't') out += ' ';
        else if (e == 'r') {}
        else if (e == 'u' && i + 4 < j.size()) {
            unsigned cp = unsigned(std::strtoul(j.substr(i + 1, 4).c_str(), nullptr, 16));
            i += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && i + 6 < j.size() && j[i + 1] == '\\' && j[i + 2] == 'u') {
                const unsigned lo = unsigned(std::strtoul(j.substr(i + 3, 4).c_str(), nullptr, 16));
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i += 6;
            }
            if (cp < 0x80) out += char(cp);
            else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 63)); }
            else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63)); }
            else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 63)); out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63)); }
        } else out += e;
    }
    return out;
}

// Release notes (Markdown) as plain lines for the game's font.
std::string plain_notes(const std::string& md) {
    std::string out;
    std::istringstream in(md);
    for (std::string line; std::getline(in, line);) {
        size_t a = line.find_first_not_of("# ");
        line = a == std::string::npos ? "" : (line[0] == '#' ? line.substr(a) : line);
        std::string t;
        for (size_t i = 0; i < line.size(); ++i) {
            const char ch = line[i];
            if (ch == '*' || ch == '`') continue;
            if (ch == '[') {                                   // [text](url) -> text
                const size_t close = line.find("](", i);
                const size_t end = close == std::string::npos ? close : line.find(')', close);
                if (end != std::string::npos) { t += line.substr(i + 1, close - i - 1); i = end; continue; }
            }
            t += ch;
        }
        for (auto [from, to] : std::initializer_list<std::pair<const char*, const char*>>{
                 {"\u2192", "->"}, {"\u2026", "..."}, {"\u2013", "-"}, {"\u2014", "-"}, {"\u00d7", "X"},
                 {"\u2022", "-"}}) {
            for (size_t q; (q = t.find(from)) != std::string::npos;) t.replace(q, std::strlen(from), to);
        }
        out += t + "\n";
    }
    return out;
}

// ------------------------------------------------- settings.toml / mods
void write_settings(const Ctx& c) {
    const fs::path path = c.game_dir / "settings.toml";
    std::map<std::string, std::map<std::string, std::string>> want;   // section -> key -> toml value
    for (const char* k : kVideoKeys) {
        const std::string key = k;
        std::string v = c.store.get(key);
        if (key == "scanlines") v = (c.store.b("scanlines") || c.store.b("crt_sim")) ? "true" : "false";
        if (key == "aspect_ratio") v = "4:3";                          // 16:9 comes from hercules.widescreen
        if (key == "window_width" && v == "0") { want["video"]["window_width"] = ""; continue; }
        want["video"][key] = toml_value(key, v);
    }
    want["video"]["auto_skip_fmv"] = "false";   // hercules.skip-intro's skip-fmv does it (the runtime leaves it to a mod)
    want["video"]["renderer"] = "\"opengl\"";
    want["audio"]["frequency"] = c.store.get("frequency");
    want["audio"]["spu_hq"] = c.store.get("spu_hq");
    want["launcher"]["skip_launcher"] = "true";
    // Player 1: the first gamepad (release builds otherwise default to the
    // keyboard); PSX_P1_KEYBOARD_TOO keeps the keyboard working alongside it.
    want["controller"]["p1_device"] = "\"auto\"";
    // merge into the existing file: replace our keys, keep everything else
    std::istringstream in(read_text(path));
    std::string out, section;
    std::set<std::string> done;
    auto flush_section = [&](const std::string& sec) {
        auto it = want.find(sec);
        if (it == want.end()) return;
        for (const auto& [k, v] : it->second)
            if (!v.empty() && !done.count(sec + "." + k)) { out += k + " = " + v + "\n"; done.insert(sec + "." + k); }
    };
    for (std::string line; std::getline(in, line);) {
        const std::string t = trim(line);
        if (!t.empty() && t[0] == '[') {
            flush_section(section);
            section = trim(t.substr(1, t.find(']') - 1));
            out += line + "\n";
            continue;
        }
        const size_t eq = t.find('=');
        if (eq != std::string::npos && t[0] != '#') {
            const std::string k = trim(t.substr(0, eq));
            auto it = want.find(section);
            if (it != want.end() && it->second.count(k)) {
                const std::string& v = it->second.at(k);
                if (!v.empty()) out += k + " = " + v + "\n";
                done.insert(section + "." + k);
                continue;
            }
        }
        out += line + "\n";
    }
    flush_section(section);
    for (const auto& [sec, keys] : want) {
        bool any = false;
        for (const auto& [k, v] : keys) any |= !v.empty() && !done.count(sec + "." + k);
        if (!any) continue;
        out += "\n[" + sec + "]\n";
        flush_section(sec);
    }
    write_atomic(path, out);
}

// HD off with a pack present: an empty pack, so hercules.hd-sprites still loads
// and the in-game menu can switch HD on without a restart.
fs::path pack_none(const Ctx& c) {
    const fs::path p = pack_base(c).parent_path() / (c.id + ".launcher-none");
    std::error_code ec;
    fs::create_directories(p / "replacements", ec);
    if (!fs::exists(p / "config.yaml")) write_atomic(p / "config.yaml", "MaxVRAMWriteSplits: 64\n");
    return p;
}

void write_mod_state(const Ctx& c, const fs::path& chosen, bool new_game) {
    const std::string level = new_game ? c.store.get("start_level") : "off";
    const bool wide = c.store.get("aspect_ratio") == "16:9";
    const fs::path pack = !chosen.empty() ? chosen : hd_pack_ready(c) ? pack_none(c) : fs::path{};
    std::string t = "format_version = 2\n";
    for (const char* p : {"hercules.hd-sprites", "psx.enhancement.hd-textures", "hercules.level-select",
                          "hercules.widescreen", "hercules.skip-intro", "psx.enhancement.cd-speed",
                          "hercules.frame-blend"})
        t += std::string("\n[[package]]\nid = \"") + p + "\"\n";
    auto feature = [&](const char* pkg, const char* id, bool on) {
        t += std::string("\n[[feature]]\npackage_id = \"") + pkg + "\"\nid = \"" + id + "\"\nenabled = " +
             (on ? "true" : "false") + "\n";
    };
    feature("hercules.hd-sprites", "hd-sprites", !pack.empty());
    if (!pack.empty()) t += "\n[feature.resources]\npack = " + toml_str(pack.generic_u8string()) + "\n";
    feature("psx.enhancement.hd-textures", "hd-textures", false);
    feature("hercules.level-select", "level-select", level == "0");
    t += "\n[feature.values]\nlevel = " + toml_str(level) + "\n";
    feature("hercules.widescreen", "widescreen", true);          // 4:3 too: the menu switches live
    t += std::string("\n[feature.values]\naspect = ") + toml_str(wide ? "16:9" : "4:3") + "\n";
    feature("hercules.skip-intro", "skip-intro", c.store.b("skip_intro"));
    feature("hercules.skip-intro", "skip-memcard", c.store.b("skip_memcard"));
    feature("hercules.skip-intro", "skip-fmv", c.store.b("auto_skip_fmv"));   // the runtime leaves it to a mod
    feature("hercules.ntsc-speed", "ntsc-speed", c.store.b("ntsc_speed"));    // 30 game steps/s like the US release
    feature("hercules.skip-intro", "direct-start", new_game);
    t += "\n[feature.values]\nlevel = " + toml_str(level == "off" || level == "0" ? "1" : level) + "\n";
    // Faster emulated CD (4x: a level's ~2 MB in ~2 s instead of ~8 s) breaks
    // the HD pack: the level textures (and sprite banks loaded mid-level) stay
    // SD. Off for everyone, whatever launcher.ini says (0.4.0/0.5.0 stored
    // fast_cd = true), until the texture tracking copes with it.
    feature("psx.enhancement.cd-speed", "cd-speed", false);
    t += "\n[feature.values]\nmultiplier = \"4\"\n";
    // Crossfade the game's 25 fps to the display refresh (presentation only).
    // Off in releases for now: with it the CRT filter and overlays are missing.
    const std::string blend = c.store.get("frame_blend");
    feature("hercules.frame-blend", "frame-blend", false);
    t += "\n[feature.values]\nblend = " + toml_str(blend == "adaptive" ? "adaptive" : "linear") + "\n";
    write_atomic(c.game_dir / "mods" / "state.toml", t);
}

// ------------------------------------------------------------ save states
struct State { int slot; std::string name; std::time_t mtime; Image thumb; };

fs::path saves_dir(const Ctx& c) {
    const std::string dir = c.game.count("runtime.memcard_dir") ? c.game.at("runtime.memcard_dir") : "saves";
    fs::path p = dir;
    if (p.is_relative() && c.game_dir != c.exe_dir) p = c.game_dir / p;   // lang/<id>/: its own
    else if (p.is_relative()) {
        const fs::path found = find_up(c.exe_dir, p);
        p = found.empty() ? c.exe_dir / p : found;
    }
    return p / "openbios";
}

unsigned entry_pc(const Ctx& c) {
    auto it = c.game.find("game.entry_pc");
    return it == c.game.end() ? 0x80013710u : unsigned(std::stoul(it->second, nullptr, 16));
}

fs::path slot_path(const Ctx& c, int slot, const char* ext) {
    char n[64];
    std::snprintf(n, sizeof n, "state_%08X_slot%02d.%s", entry_pc(c), slot, ext);
    return saves_dir(c) / n;
}

Image read_thumb(const fs::path& p) {
    const std::string d = read_text(p);
    if (d.size() < 12 || d.compare(0, 4, "PSTH") != 0) return {};
    uint32_t w, h;
    std::memcpy(&w, d.data() + 4, 4);
    std::memcpy(&h, d.data() + 8, 4);
    if (w > 4096 || h > 4096 || d.size() < 12 + size_t(w) * h * 4) return {};
    Image img = Image::make(int(w), int(h));
    for (size_t i = 0; i < size_t(w) * h; ++i) {     // BGRA -> RGBA
        const uint8_t* s = reinterpret_cast<const uint8_t*>(d.data()) + 12 + i * 4;
        uint8_t* o = &img.rgba[i * 4];
        o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; o[3] = 255;
    }
    return img;
}

std::vector<State> list_states(const Ctx& c) {
    std::vector<State> out;
    for (int slot = 0; slot < 12; ++slot) {
        const fs::path p = slot_path(c, slot, "pst");
        std::error_code ec;
        if (!fs::exists(p, ec)) continue;
        const auto ft = fs::last_write_time(p, ec);
        const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
        State s{slot, "", std::chrono::system_clock::to_time_t(sys), read_thumb(slot_path(c, slot, "thumb"))};
        const std::string nm = c.store.get("slot_name." + std::to_string(slot));
        const std::string nt = c.store.get("slot_mtime." + std::to_string(slot));
        if (!nm.empty() && !nt.empty() && std::llabs(std::atoll(nt.c_str()) - s.mtime) < 2) s.name = nm;
        out.push_back(std::move(s));
    }
    std::sort(out.begin(), out.end(), [](const State& a, const State& b) { return a.mtime > b.mtime; });
    return out;
}

// --------------------------------------------------------------- the art
struct Art {
    Font font;
    std::map<std::string, Image> bg;                  // menu room beams plaster plaster2
    std::map<std::string, Image> fresco;               // "1".."11"
    std::vector<Image> menu, muse_a, muse_b, muse_c, meg;
    std::map<std::string, Image> caption, level_name;
};

bool load_art(const Disc& d, const std::string& release, Art& a, std::string* err) {
    const uint32_t dist_table = hl::release(release).dist_table;
    std::vector<uint8_t> b, e1;
    auto pic = [&](const char* file, int index, Image& out) {
        if (!d.read(file, b)) return false;
        auto ims = decode_fullscreen(b);
        if (int(ims.size()) <= index) return false;
        out = std::move(ims[index]);
        return true;
    };
    bool ok = pic("TI/TME.0", 0, a.bg["menu"]) && pic("TI/TPS.0", 0, a.bg["room"]) &&
              pic("TI/TPS.0", 1, a.bg["beams"]) && pic("TI/T03.0", 0, a.bg["plaster"]) &&
              pic("TI/T02.0", 0, a.bg["plaster2"]);
    for (int i = 1; i <= 11; ++i) pic(("TI/TSE" + std::to_string(i) + ".0").c_str(), 0, a.fresco[std::to_string(i)]);
    if (!ok || !d.read("EX/ENGINE1.", e1)) { if (err) *err = "this is not the Hercules disc"; return false; }
    auto sprites = [&](const char* file, SpriteFrames& sf) {
        return d.read(file, b) && decode_sprites(b, e1, dist_table, sf, err);
    };
    SpriteFrames s1, s2, s3, s5, s8, s15, s16;
    if (!sprites("SD4/S4.1", s1) || !sprites("SD4/S4.2", s2) || !sprites("SD4/S4.3", s3) ||
        !sprites("SD4/S4.5", s5) || !sprites("SD4/S4.8", s8) || !sprites("SD4/S4.15", s15) ||
        !sprites("SD4/S4.16", s16))
        return false;
    for (int f = 2; f <= 5; ++f) if (s1.frames.count(f)) a.menu.push_back(s1.frames[f]);
    auto tall = [](SpriteFrames& sf, std::vector<Image>& out) {
        for (int k : sf.order()) if (sf.frames[k].h >= 120) out.push_back(sf.frames[k]);
    };
    tall(s5, a.muse_a); tall(s8, a.muse_b); tall(s15, a.muse_c);
    {
        const auto ord = s3.order();
        for (size_t i = 1; i < ord.size() && i < 25; ++i) a.meg.push_back(s3.frames[ord[i]]);
    }
    const std::pair<const char*, int> caps[] = {{"load", 1}, {"empty", 31}, {"load_help", 35}};
    for (auto [k, n] : caps) if (s16.frames.count(n)) a.caption[k] = s16.frames[n].crop_alpha();
    if (s2.frames.count(1)) a.caption["back_help"] = s2.frames[1].crop_alpha();
    const std::pair<const char*, int> names[] = {{"0", 16}, {"1", 17}, {"2", 17}, {"3", 18}, {"5", 19}, {"6", 20},
                                                 {"7", 21}, {"8", 22}, {"9", 23}, {"10", 24}, {"11", 25}};
    for (auto [k, n] : names) if (s16.frames.count(n)) a.level_name[k] = s16.frames[n].crop_alpha();
    if (!d.read("STP/STP1.", b) || !build_font(b, a.font, err, release)) return false;
    return true;
}

// ---------------------------------------------------------------- the app
enum class Key { None, Up, Down, Left, Right, Ok, Back, Delete };

struct Hit { SDL_FRect r; std::function<void()> hover, click; };

struct App;
std::string system_language();   // first known language of the system (SDL)
std::string disc_release(const fs::path& p);
std::string disc_none_message(App& a, const fs::path& p);
fs::path find_disc(const Ctx& c, const std::string& id);
void remember_disc(const Ctx& c, const fs::path& p, const std::string& id);
void use_release(Ctx& c, const std::string& id);
fs::path pick_disc(SDL_Window* win);
struct Screen {
    virtual ~Screen() = default;
    virtual void render(App&) = 0;
    virtual void key(App&, Key) = 0;
};

struct App {
    Ctx ctx;
    Art art;
    SDL_Window* win = nullptr;
    SDL_Renderer* ren = nullptr;
    SDL_Texture* stage = nullptr;
    std::map<const Image*, SDL_Texture*> tex;
    std::vector<Hit> hits;
    std::vector<Screen*> history;
    Screen* screen = nullptr;
    std::vector<State> states;
    bool quit = false, running = false;
    std::string message;
    uint64_t t0 = 0;
    SDL_Gamepad* pad = nullptr;
    Key pad_prev = Key::None;
    SDL_Process* hd_proc = nullptr;
    HdStatus hd;
    uint64_t hd_polled = 0;
    bool hd_gave_up = false;            // failed or impossible this session; the next start retries
    SDL_Process* upd_proc = nullptr;    // updater check / download
    std::string upd_op, upd_latest, upd_notes, upd_state;   // state: "", ok, download, unpack, ready, failed
    bool upd_newer = false;
    double upd_fraction = 0;
    uint64_t upd_polled = 0;

    // ---------------------------------------------------------- drawing
    SDL_Texture* texture(const Image& img) {
        auto it = tex.find(&img);
        if (it != tex.end()) return it->second;
        SDL_Texture* t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, img.w, img.h);
        SDL_UpdateTexture(t, nullptr, img.rgba.data(), img.w * 4);
        SDL_SetTextureScaleMode(t, SDL_SCALEMODE_NEAREST);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        tex[&img] = t;
        return t;
    }
    void forget(const Image& img) {
        auto it = tex.find(&img);
        if (it != tex.end()) { SDL_DestroyTexture(it->second); tex.erase(it); }
    }
    void draw(const Image& img, float x, float y, float w = -1, float h = -1, bool flip = false) {
        if (img.empty()) return;
        SDL_FRect d{x * SC, y * SC, (w < 0 ? img.w : w) * SC, (h < 0 ? img.h : h) * SC};
        SDL_RenderTextureRotated(ren, texture(img), nullptr, &d, 0, nullptr, flip ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE);
    }
    void centered(const Image& img, float y) { draw(img, std::round((W - img.w) / 2.0f), y); }
    void rect(float x, float y, float w, float h, SDL_Color c) {
        SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
        SDL_FRect r{x * SC, y * SC, w * SC, h * SC};
        SDL_RenderFillRect(ren, &r);
    }
    void frame(float x, float y, float w, float h, SDL_Color border) {
        rect(x - 2, y - 2, w + 4, h + 4, {23, 14, 12, 255});
        rect(x - 1, y - 1, w + 2, h + 2, border);
        rect(x, y, w, h, {23, 14, 12, 255});
    }
    void hit(float x, float y, float w, float h, std::function<void()> hover, std::function<void()> click) {
        hits.push_back({{x, y, w, h}, std::move(hover), std::move(click)});
    }
    int anim(size_t n, float fps) const {
        return n ? int((SDL_GetTicks() - t0) * fps / 1000.0f) % int(n) : 0;
    }

    // ------------------------------------------------------------- text
    // The chosen language; none chosen = the system's (looked up again after
    // every launcher.ini reload, so it is never lost and follows the system).
    std::string lang() const {
        const std::string l = ctx.store.get("language");
        if (kStrings.count(l)) return l;
        static const std::string sys = system_language();
        return sys;
    }
    std::string T(const std::string& key, const std::map<std::string, std::string>& vars = {}) const {
        const auto& m = kStrings.at(lang());
        auto it = m.find(key);
        std::string v = it == m.end() ? key : it->second;
        for (const auto& [k, x] : vars) {
            const size_t p = v.find("{" + k + "}");
            if (p != std::string::npos) v.replace(p, k.size() + 2, x);
        }
        return v;
    }
    const std::pair<int, int>& glyph(char32_t c) const {
        auto it = art.font.glyphs.find(upper(c));
        return it == art.font.glyphs.end() ? art.font.glyphs.at(U' ') : it->second;
    }
    int text_width(const std::string& s, float f = 1) const {
        f *= art.font.scale;
        int w = 0;
        for (char32_t c : utf32(s)) w += glyph(c).second + 1;
        return int(std::max(0, w - 1) * f + 0.5f);
    }
    void text(const std::string& s, float x, float y, const char* color = "yellow", const char* align = "left",
              float f = 1) {
        const Image& atlas = std::strcmp(color, "blue") == 0 ? art.font.blue
                           : std::strcmp(color, "orange") == 0 ? art.font.orange : art.font.yellow;
        const int w = text_width(s, f);
        f *= art.font.scale;
        float cx = std::strcmp(align, "center") == 0 ? std::round(x - w / 2.0f)
                 : std::strcmp(align, "right") == 0 ? x - w : x;
        SDL_Texture* t = texture(atlas);
        for (char32_t c : utf32(s)) {
            const auto& g = glyph(c);
            SDL_FRect src{float(g.first), 0, float(g.second), float(art.font.height)};
            SDL_FRect dst{cx * SC, y * SC, float(g.second) * SC * f, float(art.font.height) * SC * f};
            SDL_RenderTexture(ren, t, &src, &dst);
            cx += (g.second + 1) * f;
        }
    }
    std::vector<std::string> wrap(const std::string& s, int max_w, float f = 1) const {
        std::vector<std::string> lines;
        std::string line, word;
        std::istringstream in(s);
        if (s.find('\n') != std::string::npos) {
            for (std::string part; std::getline(in, part);) for (auto& l : wrap(part, max_w, f)) lines.push_back(l);
            return lines;
        }
        if (text_width(s, f) <= max_w) return {s};
        while (in >> word) {
            if (!line.empty() && text_width(line + " " + word, f) > max_w) { lines.push_back(line); line = word; }
            else line = line.empty() ? word : line + " " + word;
        }
        if (!line.empty()) lines.push_back(line);
        return lines;
    }
    float text_block(const std::string& s, float x, float y, const char* color = "yellow", int max_w = 280) {
        const auto lines = wrap(s, max_w);
        // 15 px per line; a line with accented capitals (É Ü Ñ ...: any
        // non-ASCII byte) gets 3 px more so its marks stay clear of the line above
        float dy = 0;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (i) dy += 15 + (std::any_of(lines[i].begin(), lines[i].end(),
                                           [](char ch) { return (unsigned char)ch >= 0x80; }) ? 3 : 0);
            text(lines[i], x, y + dy, color, "center");
        }
        return dy + 15.0f + (art.font.height * art.font.scale - 15);
    }
    // the game's German caption sprite, or the same words in the font
    void caption(const char* sprite, const std::string& key, float y, const char* color = "yellow") {
        auto it = art.caption.find(sprite ? sprite : "");
        if (disc_art() && it != art.caption.end()) centered(it->second, y);
        else text_block(T(key), W / 2.0f, y, color, 300);
    }
    std::string level_name(const std::string& lv) const {
        const std::string k = (lv == "off" || lv == "1") ? "2" : lv;
        for (int i = 0; i < 10; ++i) if (k == kLevelKeys[i]) return T("levels." + std::to_string(i));
        return lv;
    }

    // ---------------------------------------------------------- navigation
    void go(Screen* s, bool push = true) { if (push && screen) history.push_back(screen); screen = s; }
    void back() { if (!history.empty()) { screen = history.back(); history.pop_back(); } }
    void set(const std::string& k, const std::string& v) {
        ctx.store.kv[k] = v;
        ctx.store.save();
        if (k == "language") switch_release();
    }
    // The disc art (captions, level names, menu words) is in its disc's
    // language; elsewhere the texts are drawn in the font.
    std::string art_lang;
    bool disc_art() const { return lang() == art_lang; }
    std::string lang_name() const {
        for (const auto& l : kLangs) if (lang() == l.code) return l.name;
        return lang();
    }
    std::string release_for_ui() const {
        const Release* r = release_for_lang(lang());
        return r ? r->id : OWN_ID;
    }
    bool load_disc_art();
    void switch_release();
    bool ask_disc();
    void show_message(const std::string& m) { message = m; }

    void refresh_states() {
        for (const State& s : states) forget(s.thumb);
        states = list_states(ctx);
    }
    void play(int slot);
    void run();

    // ---------------------------------------------------------- HD build
    bool hd_wanted() const { return ctx.store.get("hd_auto") != "false"; }
    void hd_start();
    void hd_stop();
    void hd_tick();
    std::string hd_label();
    void hd_toggle();
    void hd_line(float y);

    // ----------------------------------------------------------- updates
    bool upd_spawn(const std::string& op);
    void upd_tick();
    void upd_install();
    void upd_restart();
    void upd_line(float y);
};

// ================================================================ screens
struct MainMenu : Screen {
    int sel = 0;
    void render(App& a) override;
    void key(App& a, Key k) override;
    void activate(App& a);
};
struct LevelScreen : Screen { void render(App& a) override; void key(App& a, Key k) override; };
struct OptionsScreen : Screen {
    int page = 0, row = 0;
    void render(App& a) override;
    void key(App& a, Key k) override;
    void step(App& a, int d);
};
struct LoadScreen : Screen {
    int i = 0;
    bool confirm = false, yes = false;
    void render(App& a) override;
    void key(App& a, Key k) override;
};

struct UpdateScreen : Screen {
    int choice = 0, scroll = 0;
    void render(App& a) override;
    void key(App& a, Key k) override;
};

MainMenu g_main;
UpdateScreen g_update;
LevelScreen g_levels;
OptionsScreen g_options;
LoadScreen g_load;

// Where the title menu's anchor lands on the 320x256 stage (the German
// image, anchor 110,20, sat at 52,98).
constexpr int kMenuAnchorX = 162, kMenuAnchorY = 118;

void MainMenu::render(App& a) {
    a.draw(a.art.bg["menu"], 0, 0, W, H);
    auto muse = [&](std::vector<Image>& f, float x, float y, float fps, bool flip = false, int offset = 0) {
        if (f.empty()) return;
        a.draw(f[(a.anim(f.size(), fps) + offset) % f.size()], x, y, -1, -1, flip);
    };
    muse(a.art.muse_a, 6, 100, 7); muse(a.art.muse_b, 40, 104, 6);
    muse(a.art.muse_c, 258, 100, 7); muse(a.art.muse_a, 224, 104, 5, true, 3);
    // The disc's menu image (one per highlighted entry) goes where the game
    // puts it: by its anchor, which sits elsewhere in every release's image
    // (the words differ in width), so the left edge differs too.
    const bool de = a.disc_art() && a.art.menu.size() == 4;
    float mx = 70, mw = 180;
    if (de) {
        const Image& m = a.art.menu[sel];
        mx = float(kMenuAnchorX - m.ax);
        mw = float(m.w);
        a.draw(m, mx, float(kMenuAnchorY - m.ay));
    }
    for (int i = 0; i < 4; ++i) {
        if (!de) a.text(a.T("menu." + std::to_string(i)), W / 2.0f, 101 + i * 26, i == sel ? "blue" : "orange", "center");
        a.hit(mx, 98 + i * 26, mw, 25, [this, i] { sel = i; }, [this, &a] { activate(a); });
    }
    const std::string lv = a.ctx.store.get("start_level");
    if (lv != "off") {
        auto it = a.art.level_name.find(lv);
        if (a.disc_art() && it != a.art.level_name.end()) a.centered(it->second, 6);
        else a.text_block(a.level_name(lv), W / 2.0f, -5, "yellow", 300);
    }
    if (!a.hd_proc && a.upd_newer) a.upd_line(225);
    else a.hd_line(225);
}
void MainMenu::key(App& a, Key k) {
    if (k == Key::Up) sel = (sel + 3) % 4;
    else if (k == Key::Down) sel = (sel + 1) % 4;
    else if (k == Key::Ok) activate(a);
}
void MainMenu::activate(App& a) {
    if (sel == 0) a.play(-1);
    else if (sel == 1) a.go(&g_levels);
    else if (sel == 2) a.go(&g_options);
    else { a.refresh_states(); a.go(&g_load); }
}

const char* const kLevels[] = {"off", "0", "2", "3", "5", "6", "7", "8", "9", "10", "11"};
const std::map<std::string, std::string> kFresco = {{"off", "6"}, {"0", "9"}, {"2", "5"}, {"3", "4"}, {"5", "1"},
                                                    {"6", "11"}, {"7", "2"}, {"8", "3"}, {"9", "10"}, {"10", "8"},
                                                    {"11", "7"}};

void LevelScreen::render(App& a) {
    a.draw(a.art.bg["room"], 0, 0, W, H);
    {   // light shafts: screen blend at 85 %
        SDL_Texture* t = a.texture(a.art.bg["beams"]);
        SDL_SetTextureBlendMode(t, SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE_MINUS_SRC_COLOR,
                                SDL_BLENDOPERATION_ADD, SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD));
        SDL_SetTextureColorMod(t, 217, 217, 217);
        a.draw(a.art.bg["beams"], 0, 0, W, H);
    }
    const std::string lv = a.ctx.store.get("start_level");
    a.text(a.T("level_title"), W / 2.0f, 2, "blue", "center");
    auto fr = a.art.fresco.find(kFresco.count(lv) ? kFresco.at(lv) : "6");
    if (fr != a.art.fresco.end() && !fr->second.empty()) {
        const float fw = 128, fh = 102, fx = (W - fw) / 2, fy = 28;
        a.frame(fx, fy, fw, fh, {107, 58, 26, 255});
        SDL_SetTextureScaleMode(a.texture(fr->second), SDL_SCALEMODE_LINEAR);
        a.draw(fr->second, fx, fy, fw, fh);
    }
    const std::string label = lv == "off" ? a.T("level_normal") : a.T("level_n", {{"n", lv}});
    auto sp = a.art.level_name.find(lv == "off" ? "1" : lv);
    if (a.disc_art() && sp != a.art.level_name.end()) {
        a.centered(sp->second, 134);
        a.text(label, W / 2.0f, std::round(134 + sp->second.h - a.art.font.height * a.art.font.scale + 15 + 6), "yellow", "center");
    } else {
        const float h = a.text_block(a.level_name(lv), W / 2.0f, 130, "yellow", 200);
        a.text(label, W / 2.0f, 130 + h + 2, "yellow", "center");
    }
    if (!a.art.meg.empty()) a.draw(a.art.meg[a.anim(a.art.meg.size(), 6)], 22, 96);
    a.text("<", 84, 68, "blue");
    a.text(">", 228, 68, "blue");
    a.hit(70, 28, 30, 102, [] {}, [this, &a] { key(a, Key::Left); });
    a.hit(222, 28, 30, 102, [] {}, [this, &a] { key(a, Key::Right); });
    a.hit(96, 28, 128, 102, [] {}, [this, &a] { key(a, Key::Ok); });
    a.caption("back_help", "help_back", 234);
}
void LevelScreen::key(App& a, Key k) {
    const std::string cur = a.ctx.store.get("start_level");
    int i = 0;
    for (int j = 0; j < 11; ++j) if (cur == kLevels[j]) i = j;
    if (k == Key::Left || k == Key::Right) a.set("start_level", kLevels[(i + (k == Key::Left ? 10 : 1)) % 11]);
    else if (k == Key::Ok) a.play(-1);
    else if (k == Key::Back) a.back();
}

// options: rows as [key, {value, label}] where label is a val.* key, on/off, or "=literal"
struct Row { std::string key; std::vector<std::pair<std::string, std::string>> choices; };
std::vector<std::pair<std::string, std::string>> pct(std::vector<double> vs) {
    std::vector<std::pair<std::string, std::string>> out;
    for (double v : vs) {
        char b[16];
        std::snprintf(b, sizeof b, "%g", v);
        out.push_back({b, "=" + std::to_string(int(std::lround(v * 100)))});
    }
    return out;
}
const std::vector<std::pair<std::string, std::string>> kOnOff = {{"true", "on"}, {"false", "off"}};
std::vector<std::pair<std::string, std::vector<Row>>> pages() {
    std::vector<std::pair<std::string, std::string>> langs;
    for (const auto& l : kLangs) langs.push_back({l.code, std::string("=") + l.name});
    return {
        {"general", {{"language", langs}, {"skip_intro", {{"false", "show"}, {"true", "skip"}}},
                     {"ntsc_speed", {{"false", "=PAL"}, {"true", "=NTSC"}}}, {"updates", kOnOff}}},
        {"video", {{"internal_resolution", {{"native", "native"}, {"720p", "=720P"}, {"1080p", "=1080P"},
                                            {"1440p", "=1440P"}, {"4k", "=4K"}}},
                   {"aspect_ratio", {{"4:3", "ratio43"}, {"16:9", "ratio169"}}},
                   {"fullscreen", {{"0", "window"}, {"1", "full"}, {"2", "exclusive"}}},
                   {"vsync", {{"on", "on"}, {"adaptive", "adaptive"}, {"vrr", "=VRR"}, {"off", "off"}}},
                   {"preset", {{"original", "p_original"}, {"hd", "p_hd"}, {"custom", "p_custom"}}}}},
        // frame_blend ("off"/"linear"/"adaptive") is hidden until CRT and overlays work with it
        {"gfx", {{"hd_build", {}}, {"hd_sprites", kOnOff}, {"hd_levels", kOnOff}, {"hd_hud", kOnOff},
                 {"texture_filtering", {{"nearest", "sharp"}, {"bilinear", "soft"}, {"xbr", "=XBR"}}},
                 {"antialiasing", kOnOff}, {"perspective_texturing", kOnOff}, {"geometry_correction", kOnOff}}},
        {"filter", {{"crt_sim", kOnOff}, {"crt_mask", pct({0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1})},
                    {"crt_bloom", pct({0, 0.1, 0.2, 0.3, 0.35, 0.4, 0.5, 0.6, 0.8, 1})}, {"scanlines", kOnOff},
                    {"scanline_strength", pct({0.1, 0.2, 0.3, 0.35, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1})},
                    {"crt_filter", {{"raw", "raw"}, {"crt", "tube"}, {"composite", "=COMPOSITE"}, {"trinitron", "=TRINITRON"}}}}},
        {"sound", {{"frequency", {{"48000", "=48000"}, {"44100", "=44100"}, {"32000", "=32000"}}},
                   {"spu_hq", {{"true", "high"}, {"false", "normal"}}},
                   {"auto_skip_fmv", {{"false", "show"}, {"true", "skip"}}}, {"rewind", kOnOff}}},
    };
}
std::string current(App& a, const std::string& key) {
    if (key == "language") return a.lang();     // none chosen: shows the system's
    return key == "preset" ? a.ctx.preset() : a.ctx.store.get(key);
}
int choice_index(const std::string& v, const std::vector<std::pair<std::string, std::string>>& ch) {
    for (size_t i = 0; i < ch.size(); ++i) if (ch[i].first == v) return int(i);
    // numbers stored with other formatting: nearest
    char* end = nullptr;
    const double x = std::strtod(v.c_str(), &end);
    if (end && end != v.c_str()) {
        int best = 0;
        double bd = 1e9;
        for (size_t i = 0; i < ch.size(); ++i) {
            const double d = std::fabs(std::strtod(ch[i].first.c_str(), nullptr) - x);
            if (d < bd) { bd = d; best = int(i); }
        }
        return best;
    }
    return 0;
}
std::string choice_label(App& a, const std::string& label) {
    if (!label.empty() && label[0] == '=') return label.substr(1);
    if (label == "on" || label == "off") return a.T(label);
    return a.T("val." + label);
}

void OptionsScreen::render(App& a) {
    a.draw(a.art.bg["plaster"], 0, 0, W, H);
    const auto ps = pages();
    const auto& [page_key, rows] = ps[page];
    const std::string title = a.T("pages." + page_key);
    a.text(a.T("options"), W / 2.0f, 4, "yellow", "center");
    a.text("< " + title + " >", W / 2.0f, 28, row == 0 ? "blue" : "orange", "center");
    a.hit(60, 30, 200, 18, [this] { row = 0; }, [this, &a] { step(a, 1); });
    for (size_t i = 0; i < rows.size(); ++i) {
        const float y = 54 + i * 20;
        const bool active = row == int(i) + 1;
        a.text(a.T("opt." + rows[i].key), 20, y, active ? "blue" : "yellow");
        const auto& ch = rows[i].choices;
        a.text(rows[i].key == "hd_build" ? a.hd_label() : choice_label(a, ch[choice_index(current(a, rows[i].key), ch)].second),
               300, y, active ? "blue" : "orange", "right");
        a.hit(14, y + 4, 292, 18, [this, i] { row = int(i) + 1; }, [this, &a, i] { row = int(i) + 1; step(a, 1); });
    }
    a.caption("back_help", "help_back", 234);
}
void OptionsScreen::step(App& a, int d) {
    const auto ps = pages();
    if (row == 0) { page = (page + d + int(ps.size())) % int(ps.size()); return; }
    const Row& r = ps[page].second[row - 1];
    if (r.key == "hd_build") { a.hd_toggle(); return; }
    const int n = int(r.choices.size());
    const std::string nv = r.choices[(choice_index(current(a, r.key), r.choices) + d + n) % n].first;
    if (r.key == "preset") {
        if (nv != "custom") for (const auto& [k, v] : kPresets.at(nv)) a.ctx.store.kv[k] = v;
        a.ctx.store.save();
    } else {
        a.set(r.key, nv);
    }
}
void OptionsScreen::key(App& a, Key k) {
    const int n = int(pages()[page].second.size()) + 1;
    if (k == Key::Up) row = (row + n - 1) % n;
    else if (k == Key::Down) row = (row + 1) % n;
    else if (k == Key::Left) step(a, -1);
    else if (k == Key::Right || k == Key::Ok) step(a, 1);
    else if (k == Key::Back) a.back();
}

std::string fmt_time(App& a, std::time_t t) {
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char b[64];
    const std::string l = a.lang();
    std::strftime(b, sizeof b, l == "de" ? "%d.%m.%Y %H:%M" : l == "en" ? "%Y-%m-%d %H:%M" : "%d/%m/%Y %H:%M", &tm);
    return b;
}

void LoadScreen::render(App& a) {
    a.draw(a.art.bg["plaster2"], 0, 0, W, H);
    a.caption("load", "load_title", 6);
    auto& st = a.states;
    if (st.empty()) {
        a.caption("empty", "empty", 110);
        a.caption("back_help", "help_back", 234);
        return;
    }
    i = std::min(i, int(st.size()) - 1);
    auto show = [&](int k, float x, float y, float w, float h, bool on) {
        a.frame(x, y, w, h, on ? SDL_Color{140, 185, 243, 255} : SDL_Color{107, 58, 26, 255});
        if (!st[k].thumb.empty()) {
            SDL_Texture* t = a.texture(st[k].thumb);
            SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
            SDL_SetTextureAlphaMod(t, on ? 255 : 140);
            a.draw(st[k].thumb, x, y, w, h);
        }
        a.hit(x, y, w, h, [] {}, [this, &a, k] { if (k == i) key(a, Key::Ok); else i = k; });
    };
    if (i > 0) show(i - 1, 12, 74, 64, 48, false);
    if (i < int(st.size()) - 1) show(i + 1, 244, 74, 64, 48, false);
    show(i, 96, 50, 128, 96, true);
    const State& s = st[i];
    a.text(s.name.empty() ? a.T("slot", {{"n", std::to_string(s.slot + 1)}}) : s.name, W / 2.0f, 148, "blue", "center");
    a.text(fmt_time(a, s.mtime), W / 2.0f, 166, "yellow", "center");
    a.text(a.T("of", {{"i", std::to_string(i + 1)}, {"n", std::to_string(st.size())}}), W / 2.0f, 184, "orange", "center");
    a.text_block(a.T("help_load"), W / 2.0f, 204, "yellow", 300);
    if (confirm) {
        a.rect(0, 0, W, H, {16, 4, 3, 158});
        a.text(a.T("delete_q"), W / 2.0f, 92, "yellow", "center");
        a.text(a.T("yes"), 120, 120, yes ? "blue" : "yellow", "center");
        a.text(a.T("no"), 200, 120, yes ? "yellow" : "blue", "center");
        a.hit(96, 124, 48, 20, [this] { yes = true; }, [this, &a] { yes = true; key(a, Key::Ok); });
        a.hit(172, 124, 56, 20, [this] { yes = false; }, [this, &a] { yes = false; key(a, Key::Ok); });
    }
}
void LoadScreen::key(App& a, Key k) {
    auto& st = a.states;
    if (confirm) {
        if (k == Key::Left || k == Key::Right) yes = !yes;
        else if (k == Key::Back) confirm = false;
        else if (k == Key::Ok) {
            confirm = false;
            if (yes && i < int(st.size())) {
                std::error_code ec;
                for (const char* e : {"pst", "thumb", "hdres"}) fs::remove(slot_path(a.ctx, st[i].slot, e), ec);
                a.ctx.store.kv.erase("slot_name." + std::to_string(st[i].slot));
                a.ctx.store.save();
                a.refresh_states();
            }
        }
        return;
    }
    const int n = int(st.size());
    if (k == Key::Left && n) i = (i + n - 1) % n;
    else if (k == Key::Right && n) i = (i + 1) % n;
    else if (k == Key::Ok && n) a.play(st[i].slot);
    else if (k == Key::Delete && n) { confirm = true; yes = false; }
    else if (k == Key::Back) a.back();
}

// ------------------------------------------------------------------- play
void App::play(int slot) {
    if (running) return;
    // the chosen language's release: its disc first (ask for it), then its build
    if (ctx.disc.empty() && !ask_disc()) return;
    if (ctx.exe.empty()) { show_message(T("disc.no_build", {{"lang", lang_name()}})); return; }
    const fs::path pack = pack_for(ctx);
    write_settings(ctx);
    write_mod_state(ctx, pack, slot < 0);
    SDL_Environment* env = SDL_CreateEnvironment(true);
    for (const char* k : {"PSX_LOAD_SLOT", "PSX_SCANLINES", "PSX_SCANLINE_STRENGTH", "PSX_NO_LAUNCHER", "PSX_VOLUME"})
        SDL_UnsetEnvironmentVariable(env, k);
    if (slot >= 0) SDL_SetEnvironmentVariable(env, "PSX_LOAD_SLOT", std::to_string(slot).c_str(), true);
    SDL_SetEnvironmentVariable(env, "PSX_P1_KEYBOARD_TOO", "1", true);
    // a build in lang/<id>/ keeps using the game folder's settings and keys
    SDL_SetEnvironmentVariable(env, "PSX_HL_HOME", ctx.exe_dir.u8string().c_str(), true);
    SDL_SetEnvironmentVariable(env, "PSX_KEYBINDS_INI", (ctx.exe_dir / "keybinds.ini").u8string().c_str(), true);
    SDL_SetEnvironmentVariable(env, "PSX_CRT", ctx.store.b("crt_sim") ? "1" : "0", true);
    SDL_SetEnvironmentVariable(env, "PSX_CRT_MASK", ctx.store.get("crt_mask").c_str(), true);
    SDL_SetEnvironmentVariable(env, "PSX_CRT_BLOOM", ctx.store.get("crt_bloom").c_str(), true);
    if (!ctx.store.get("volume").empty())
        SDL_SetEnvironmentVariable(env, "PSX_VOLUME", ctx.store.get("volume").c_str(), true);
    const std::string exe = ctx.exe.u8string(), disc = ctx.disc.u8string();
    const char* args[] = {exe.c_str(), "--no-launcher", "--disc", disc.c_str(), nullptr};
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void*)args);
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, env);
    const std::string cwd = ctx.game_dir.u8string();
    SDL_SetStringProperty(props, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, cwd.c_str());
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_INHERITED);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_INHERITED);
    SDL_Process* proc = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    SDL_DestroyEnvironment(env);
    if (!proc) { show_message(T("err.start_failed")); return; }
    running = true;
    SDL_HideWindow(win);
    int code = 0;
    while (!SDL_WaitProcess(proc, false, &code)) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) if (e.type == SDL_EVENT_QUIT) {}
        SDL_Delay(100);
    }
    SDL_DestroyProcess(proc);
    running = false;
    SDL_ShowWindow(win);
    SDL_RaiseWindow(win);
    ctx.store.load();                          // changed in the in-game menu
    refresh_states();
    if (hd_wanted() && !ctx.store.b("hd_started")) set("hd_started", "true");   // HD build starts now
    if (code) show_message(T("err.exit", {{"n", std::to_string(code)}}));
}

// --------------------------------------------------------- the releases
// The art of the chosen release's disc, else of any disc the player gave.
bool App::load_disc_art() {
    fs::path p = ctx.disc;
    std::string id = ctx.id;
    for (const auto& r : kReleases) {
        if (!p.empty()) break;
        p = find_disc(ctx, r.id);
        id = r.id;
    }
    if (p.empty()) return false;
    Disc d;
    Art fresh;
    std::string err;
    if (!d.open(p, &err) || !load_art(d, id, fresh, &err)) {
        std::fprintf(stderr, "launcher: no art from %s: %s\n", p.u8string().c_str(), err.c_str());
        return false;
    }
    // one look in every language: the German strip has the most letters
    const fs::path de = id == "SCES-00893" ? fs::path{} : find_disc(ctx, "SCES-00893");
    Disc dd;
    std::vector<uint8_t> stp;
    Font font;
    if (!de.empty() && dd.open(de, &err) && dd.read("STP/STP1.", stp) && build_font(stp, font, &err))
        fresh.font = std::move(font);
    for (auto& [img, t] : tex) SDL_DestroyTexture(t);
    tex.clear();
    art = std::move(fresh);
    art_lang = release(id).lang;
    return true;
}

// The language picks the release: its build, disc, HD pack and saves.
void App::switch_release() {
    const std::string id = release_for_ui();
    if (id == ctx.id) return;
    hd_stop();
    use_release(ctx, id);
    hd = hd_read(ctx);
    hd_gave_up = false;
    refresh_states();
    load_disc_art();
}

// No disc yet for the chosen release: say which one, let the player pick it.
// A disc of another release is remembered for that one.
bool App::ask_disc() {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Hercules Recompiled",
                             T("disc.need", {{"lang", lang_name()}, {"id", ctx.id}}).c_str(), win);
    const fs::path p = pick_disc(win);
    if (p.empty()) return false;
    const std::string got = disc_release(p);
    if (got.empty()) { show_message(disc_none_message(*this, p)); return false; }
    remember_disc(ctx, p, got);
    if (got != ctx.id) {
        show_message(T("disc.wrong", {{"got", got}, {"lang", lang_name()}, {"id", ctx.id}}));
        return false;
    }
    ctx.disc = p;
    load_disc_art();
    return true;
}

// ------------------------------------------------------------ HD build
void App::hd_start() {
    const fs::path py = hd_python(ctx), script = ctx.exe_dir / "hd" / "hd_auto.py";
    if (hd_proc || ctx.disc.empty() || !fs::exists(py) || !fs::exists(script)) { hd_gave_up = true; return; }
#ifdef _WIN32
    const std::string pid = std::to_string(GetCurrentProcessId());
#else
    const std::string pid = std::to_string(getpid());
#endif
    const std::string a_py = py.u8string(), a_script = script.u8string(), a_disc = ctx.disc.u8string(),
                      a_pack = pack_base(ctx).u8string(), a_data = hd_data(ctx).u8string();
    const char* args[] = {a_py.c_str(), a_script.c_str(), "--disc", a_disc.c_str(), "--pack", a_pack.c_str(),
                          "--data", a_data.c_str(), "--parent-pid", pid.c_str(), nullptr};
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void*)args);
    const std::string cwd = ctx.exe_dir.u8string();
    SDL_SetStringProperty(props, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, cwd.c_str());
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
    hd_proc = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (!hd_proc) hd_gave_up = true;
    hd_polled = 0;
}

void App::hd_stop() {
    if (!hd_proc) return;
    SDL_KillProcess(hd_proc, false);           // it stops its own steps; images made so far stay
    SDL_WaitProcess(hd_proc, true, nullptr);
    SDL_DestroyProcess(hd_proc);
    hd_proc = nullptr;
    hd = hd_read(ctx);
}

void App::hd_tick() {
    const uint64_t now = SDL_GetTicks();
    if (hd_polled && now - hd_polled < 1000) return;
    hd_polled = now;
    if (hd_proc) {
        int code = 0;
        if (!SDL_WaitProcess(hd_proc, false, &code)) { hd = hd_read(ctx); return; }
        SDL_DestroyProcess(hd_proc);
        hd_proc = nullptr;
        hd = hd_read(ctx);
        if (hd.state == "done" && hd_pack_ready(ctx)) {
            show_message(T("hd_done"));
        } else {
            hd_gave_up = true;
            if (hd.state == "failed")
                show_message(T(hd.error == "network" || hd.error == "disk" ? "hd_err_" + hd.error : "hd_err_failed"));
        }
        return;
    }
    if (!hd_gave_up && hd_wanted() && ctx.store.b("hd_started") && !hd_pack_ready(ctx)) hd_start();
}

std::string App::hd_label() {
    const std::string p = std::to_string(int(hd.fraction * 100));
    if (hd_pack_ready(ctx) && !hd_proc) return T("val.hd_ready");
    if (hd_proc) return p + " %";
    if (!fs::exists(ctx.exe_dir / "hd" / "hd_auto.py")) return T("val.hd_unavailable");
    if (!hd.state.empty()) return T("val.hd_paused", {{"p", p}});
    return T("val.hd_make");
}

void App::hd_toggle() {
    if (hd_proc) { hd_stop(); set("hd_auto", "false"); return; }
    if (hd_pack_ready(ctx)) return;
    set("hd_auto", "true");
    set("hd_started", "true");
    hd_gave_up = false;
    hd_start();
}

// bottom of the main menu while the pack is being made
void App::hd_line(float y) {
    if (!hd_proc) return;
    const int p = int(hd.fraction * 100);
    std::string msg;
    if (hd.phase == "build" && hd.eta >= 0) {
        const long m = std::max(1L, std::lround(hd.eta / 60.0));
        char eta[32];
        if (m < 60) std::snprintf(eta, sizeof eta, "%ld MIN", m);
        else std::snprintf(eta, sizeof eta, "%ld:%02ld H", m / 60, m % 60);
        msg = T("hd_progress", {{"p", std::to_string(p)}, {"eta", eta}});
    } else if (hd.phase == "download" || hd.phase == "install") {   // the ready pack (hd_auto.py)
        msg = T("hd_download", {{"p", std::to_string(p)}});
    } else {
        msg = T("hd_setup", {{"p", std::to_string(p)}});
    }
    text(msg, W / 2.0f, y, "yellow", "center");
    const float bw = 200, bx = (W - bw) / 2, by = y + 24;
    rect(bx - 1, by - 1, bw + 2, 6, {107, 58, 26, 255});
    rect(bx, by, bw, 4, {20, 8, 4, 220});
    rect(bx, by, bw * float(hd.fraction), 4, {255, 208, 56, 255});
}

Key key_of(SDL_Keycode k) {
    switch (k) {
    case SDLK_UP: return Key::Up;
    case SDLK_DOWN: return Key::Down;
    case SDLK_LEFT: return Key::Left;
    case SDLK_RIGHT: return Key::Right;
    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: case SDLK_X: return Key::Ok;
    case SDLK_ESCAPE: case SDLK_BACKSPACE: return Key::Back;
    case SDLK_DELETE: case SDLK_Q: return Key::Delete;
    default: return Key::None;
    }
}

Key pad_state(SDL_Gamepad* p) {
    if (!p) return Key::None;
    const int ax = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTX), ay = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTY);
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_UP) || ay < -20000) return Key::Up;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_DOWN) || ay > 20000) return Key::Down;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_LEFT) || ax < -20000) return Key::Left;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || ax > 20000) return Key::Right;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_SOUTH) || SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_START)) return Key::Ok;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_NORTH) || SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_EAST)) return Key::Back;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_WEST)) return Key::Delete;
    return Key::None;
}

// HL_SCRIPT="500:down 500:ok 800:shot:/tmp/a.bmp 200:quit": timed keys and
// stage screenshots, for testing the launcher without a person at the window
// (Wine cannot be screen-grabbed under Wayland).
struct ScriptStep { uint64_t at; std::string action; };
std::vector<ScriptStep> parse_script() {
    std::vector<ScriptStep> out;
    const char* env = SDL_getenv("HL_SCRIPT");
    if (!env) return out;
    std::istringstream in(env);
    uint64_t t = 0;
    for (std::string tok; in >> tok;) {
        const size_t c = tok.find(':');
        if (c == std::string::npos) continue;
        t += std::strtoull(tok.substr(0, c).c_str(), nullptr, 10);
        out.push_back({t, tok.substr(c + 1)});
    }
    return out;
}

void App::run() {
    t0 = SDL_GetTicks();
    screen = &g_main;
    auto script = parse_script();
    size_t next_step = 0;
    std::string pending_shot;
    while (!quit) {
        while (next_step < script.size() && SDL_GetTicks() - t0 >= script[next_step].at) {
            const std::string& act = script[next_step++].action;
            static const std::map<std::string, Key> keys = {{"up", Key::Up}, {"down", Key::Down}, {"left", Key::Left},
                {"right", Key::Right}, {"ok", Key::Ok}, {"back", Key::Back}, {"delete", Key::Delete}};
            if (act == "quit") quit = true;
            else if (act.rfind("shot:", 0) == 0) pending_shot = act.substr(5);
            else if (act.rfind("lang:", 0) == 0) set("language", act.substr(5));
            else if (keys.count(act)) { if (!message.empty()) message.clear(); else screen->key(*this, keys.at(act)); }
        }
        if (quit) break;
        SDL_Event e;
        SDL_FRect view{};
        {   // the 4:3 view inside the window
            int ww, wh;
            SDL_GetWindowSizeInPixels(win, &ww, &wh);
            const float vw = std::min(float(ww), wh * 4.0f / 3.0f), vh = vw * 3.0f / 4.0f;
            view = {(ww - vw) / 2, (wh - vh) / 2, vw, vh};
        }
        auto to_stage = [&](float x, float y, float& sx, float& sy) {
            float px = x, py = y;
            SDL_RenderCoordinatesFromWindow(ren, x, y, &px, &py);
            sx = (px - view.x) / view.w * W;
            sy = (py - view.y) / view.h * H;
        };
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT: quit = true; break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.key == SDLK_F11) {
                    SDL_SetWindowFullscreen(win, !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN));
                } else if (!message.empty()) {
                    message.clear();
                } else if (Key k = key_of(e.key.key); k != Key::None) {
                    screen->key(*this, k);
                }
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                if (!pad) pad = SDL_OpenGamepad(e.gdevice.which);
                break;
            case SDL_EVENT_MOUSE_MOTION:
            case SDL_EVENT_MOUSE_BUTTON_DOWN: {
                float sx, sy;
                to_stage(e.type == SDL_EVENT_MOUSE_MOTION ? e.motion.x : e.button.x,
                         e.type == SDL_EVENT_MOUSE_MOTION ? e.motion.y : e.button.y, sx, sy);
                if (!message.empty()) { if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) message.clear(); break; }
                for (auto it = hits.rbegin(); it != hits.rend(); ++it) {
                    if (sx < it->r.x || sy < it->r.y || sx >= it->r.x + it->r.w || sy >= it->r.y + it->r.h) continue;
                    if (e.type == SDL_EVENT_MOUSE_MOTION) it->hover(); else it->click();
                    break;
                }
                break;
            }
            default: break;
            }
        }
        const Key pk = pad_state(pad);
        if (pk != Key::None && pk != pad_prev) {
            if (!message.empty()) message.clear(); else screen->key(*this, pk);
        }
        pad_prev = pk;
        if (quit) break;
        hd_tick();
        upd_tick();

        // draw the stage at 4x, then fit it into the window as 4:3
        hits.clear();
        SDL_SetRenderTarget(ren, stage);
        SDL_SetRenderDrawColor(ren, 52, 9, 7, 255);
        SDL_RenderClear(ren);
        screen->render(*this);
        if (!message.empty()) {
            rect(0, 0, W, H, {16, 4, 3, 158});
            text_block(message, W / 2.0f, 96, "yellow", 280);
        }
        if (!pending_shot.empty()) {
            if (SDL_Surface* sf = SDL_RenderReadPixels(ren, nullptr)) {
                SDL_SaveBMP(sf, pending_shot.c_str());
                SDL_DestroySurface(sf);
            }
            pending_shot.clear();
        }
        SDL_SetRenderTarget(ren, nullptr);
        SDL_SetRenderDrawColor(ren, 16, 9, 7, 255);
        SDL_RenderClear(ren);
        SDL_RenderTexture(ren, stage, nullptr, &view);
        SDL_RenderPresent(ren);
        SDL_Delay(15);
    }
}


// --------------------------------------------------------------- updates
bool App::upd_spawn(const std::string& op) {
    const fs::path py = hd_python(ctx), script = upd_dir(ctx) / "updater.py";
    if (upd_proc || !fs::exists(py) || !fs::exists(script)) return false;
    const std::string a_py = py.u8string(), a_script = script.u8string(), a_game = ctx.exe_dir.u8string();
    const char* args[] = {a_py.c_str(), a_script.c_str(), op.c_str(), "--game", a_game.c_str(), nullptr};
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void*)args);
    SDL_SetStringProperty(props, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, a_game.c_str());
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
    upd_proc = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    upd_op = op;
    return upd_proc != nullptr;
}

void App::upd_tick() {
    const uint64_t now = SDL_GetTicks();
    if (!upd_proc || now - upd_polled < 250) return;
    upd_polled = now;
    if (upd_op == "download") {
        const std::string j = read_text(upd_dir(ctx) / "progress.json");
        if (!j.empty()) { upd_state = json_field(j, "state"); upd_fraction = std::atof(json_field(j, "fraction").c_str()); }
    }
    int code = 0;
    if (!SDL_WaitProcess(upd_proc, false, &code)) return;
    SDL_DestroyProcess(upd_proc);
    upd_proc = nullptr;
    if (upd_op == "check") {
        const std::string j = read_text(upd_dir(ctx) / "check.json");
        if (json_field(j, "state") != "ok") return;               // offline: say nothing
        upd_newer = json_field(j, "newer") == "true";
        upd_latest = json_field(j, "latest");
        upd_notes = plain_notes(json_field(j, "notes"));
        if (upd_newer && ctx.store.get("update_seen") != upd_latest && screen == &g_main) {
            g_update.choice = 0;
            g_update.scroll = 0;
            go(&g_update);
        }
    } else {
        const std::string j = read_text(upd_dir(ctx) / "progress.json");
        upd_state = json_field(j, "state");
        if (upd_state != "ready") upd_state = "failed";
    }
}

void App::upd_install() {
    upd_state = "download";
    upd_fraction = 0;
    if (!upd_spawn("download")) upd_state = "failed";
}

// Hand over to the new version's updater and Python (nothing it replaces is
// in use), quit; it waits for this process, copies, starts the game again.
void App::upd_restart() {
    const fs::path st = upd_dir(ctx) / "staging";
#ifdef _WIN32
    fs::path py = st / "overlay_toolchain" / "python" / "pythonw.exe";
    const std::string pid = std::to_string(GetCurrentProcessId());
#else
    fs::path py = st / "overlay_toolchain" / "python" / "bin" / "python3";
    const std::string pid = std::to_string(getpid());
#endif
    fs::path script = st / "update" / "updater.py";
    if (!fs::exists(py)) py = hd_python(ctx);
    if (!fs::exists(script)) script = upd_dir(ctx) / "updater.py";
    const std::string a_py = py.u8string(), a_script = script.u8string(), a_game = ctx.exe_dir.u8string(),
                      a_exe = ctx.launcher_exe.u8string();
    const char* args[] = {a_py.c_str(), a_script.c_str(), "apply", "--game", a_game.c_str(), "--wait-pid", pid.c_str(),
                          "--relaunch", a_exe.c_str(), nullptr};
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void*)args);
    SDL_SetStringProperty(props, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, a_game.c_str());
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
    SDL_Process* p = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (!p) { upd_state = "failed"; return; }
    SDL_DestroyProcess(p);                      // keeps running on its own
    quit = true;
}

void App::upd_line(float y) {
    text(T("upd.available", {{"v", upd_latest}}), W / 2.0f, y, "yellow", "center", 0.8f);
    hit(40, y, 240, 20, [] {}, [this] { g_update.choice = 0; g_update.scroll = 0; go(&g_update); });
}

void UpdateScreen::render(App& a) {
    a.draw(a.art.bg["plaster"], 0, 0, W, H);
    a.text(a.T("upd.title", {{"v", a.upd_latest}}), W / 2.0f, 4, "yellow", "center");
    const float f = 0.55f, lh = 12.5f;
    const auto lines = a.wrap(a.upd_notes, 290, f);
    const int vis = 12;
    scroll = std::max(0, std::min(scroll, int(lines.size()) - vis));
    for (int i = 0; i < vis && scroll + i < int(lines.size()); ++i)
        a.text(lines[size_t(scroll + i)], 15, 32 + i * lh, "yellow", "left", f);
    if (int(lines.size()) > vis) {
        if (scroll > 0) a.text("<", 306, 30, "orange", "left", 0.6f);
        if (scroll + vis < int(lines.size())) a.text(">", 306, 168, "orange", "left", 0.6f);
    }
    const std::string& st = a.upd_state;
    if (st == "download" || st == "unpack") {
        const int pct = int(a.upd_fraction * 100);
        a.text(a.T("upd.loading", {{"p", std::to_string(pct)}}), W / 2.0f, 196, "yellow", "center", 0.8f);
        a.rect(59, 221, 202, 6, {107, 58, 26, 255});
        a.rect(60, 222, 200, 4, {20, 8, 4, 220});
        a.rect(60, 222, 200 * float(a.upd_fraction), 4, {255, 208, 56, 255});
        return;
    }
    std::vector<std::string> opts;
    if (st == "ready") opts = {a.T("upd.restart"), a.T("upd.later")};
    else opts = {a.T("upd.install"), a.T("upd.later")};
    if (st == "failed") a.text(a.T("upd.failed"), W / 2.0f, 186, "orange", "center", 0.7f);
    choice = std::min(choice, int(opts.size()) - 1);
    for (int i = 0; i < int(opts.size()); ++i) {
        const float x = i == 0 ? 85 : 235;
        a.text(opts[size_t(i)], x, 210, i == choice ? "blue" : "orange", "center", 0.8f);
        a.hit(x - 70, 208, 140, 22, [this, i] { choice = i; }, [this, &a, i] { choice = i; key(a, Key::Ok); });
    }
}
void UpdateScreen::key(App& a, Key k) {
    const std::string& st = a.upd_state;
    if (st == "download" || st == "unpack") { if (k == Key::Back) a.back(); return; }
    if (k == Key::Left || k == Key::Right) choice = 1 - choice;
    else if (k == Key::Up) scroll = std::max(0, scroll - 3);
    else if (k == Key::Down) scroll += 3;
    else if (k == Key::Back || (k == Key::Ok && choice == 1)) {
        a.set("update_seen", a.upd_latest);
        a.back();
    } else if (k == Key::Ok) {
        if (st == "ready") a.upd_restart();
        else a.upd_install();
    }
}

// ---------------------------------------------------------- disc lookup
// Which PAL release of Hercules a disc image is ("" = none), from the boot
// file SYSTEM.CNF names.
std::string disc_release(const fs::path& p) {
    Disc d;
    std::string err;
    std::vector<uint8_t> cnf;
    if (!d.open(p, &err) || !d.read("SYSTEM.CNF", cnf)) return "";
    const std::string t(cnf.begin(), cnf.end());
    for (const auto& r : kReleases) if (t.find(r.boot) != std::string::npos) return r.id;
    return "";
}

// Not one of the PAL releases: say so when it is the US one (SLUS-01029),
// which the port does not support, instead of "not a Hercules disc".
std::string disc_none_message(App& a, const fs::path& p) {
    Disc d;
    std::string err;
    std::vector<uint8_t> cnf;
    if (d.open(p, &err) && d.read("SYSTEM.CNF", cnf) &&
        std::string(cnf.begin(), cnf.end()).find("SLUS_010.29") != std::string::npos)
        return a.T("disc.ntsc");
    std::fprintf(stderr, "launcher: %s: %s\n", p.u8string().c_str(), err.empty() ? "no PAL Hercules boot file" : err.c_str());
    return a.T("disc.none");
}

// One disc per release: disc.cfg (the German one, as before) or disc-<id>.cfg.
fs::path disc_cfg(const Ctx& c, const std::string& id) {
    return c.exe_dir / (id == "SCES-00893" ? std::string("disc.cfg") : "disc-" + id + ".cfg");
}

fs::path find_disc(const Ctx& c, const std::string& id) {
    auto it = c.game.find("game.disc");
    if (id == c.id && it != c.game.end() && !it->second.empty()) {
        fs::path p = fs::u8path(it->second);
        if (p.is_relative()) p = find_up(c.game_dir, p);
        if (!p.empty() && disc_release(p) == id) return p;
    }
    const std::string cached = trim(read_text(disc_cfg(c, id)));
    if (!cached.empty()) {
        fs::path p = fs::u8path(cached);
        if (p.is_relative()) p = c.exe_dir / p;
        if (disc_release(p) == id) return p;
    }
    return {};
}

void remember_disc(const Ctx& c, const fs::path& p, const std::string& id) {
    std::error_code ec;
    const fs::path rel = fs::relative(p, c.exe_dir, ec);
    write_atomic(disc_cfg(c, id), (ec || rel.empty() ? p : rel).generic_u8string() + "\n");
}

// Point the context at release id: its build (here, or lang/<id>/), its
// game.toml and its disc.
void use_release(Ctx& c, const std::string& id) {
    c.id = id;
    const fs::path sub = c.exe_dir / "lang" / id;
    c.game_dir = id == OWN_ID && !fs::exists(sub / "game.toml") ? c.exe_dir : sub;
    c.exe.clear();
    for (const char* n : {"Hercules_Recompiled.exe", "Hercules_Recompiled"})
        if (fs::exists(c.game_dir / n)) { c.exe = c.game_dir / n; break; }
    c.game_toml = c.game_dir / "game.toml";
    c.game = toml_flat(read_text(c.game_toml));
    c.disc = find_disc(c, id);
}

// SDL's native file dialog runs asynchronously; pump events until it answers.
fs::path pick_disc(SDL_Window* win) {
    struct Result { bool done = false; std::string path; } r;
    static const SDL_DialogFileFilter filters[] = {{"PlayStation disc image (*.cue, *.bin, *.img, *.iso, *.chd)", "cue;bin;img;iso;chd"}};
    SDL_ShowOpenFileDialog(
        [](void* ud, const char* const* list, int) {
            auto* res = static_cast<Result*>(ud);
            if (list && list[0]) res->path = list[0];
            res->done = true;
        },
        &r, win, filters, 1, nullptr, false);
    while (!r.done) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) if (e.type == SDL_EVENT_QUIT) return {};
        SDL_Delay(20);
    }
    return fs::u8path(r.path);
}

std::string system_language() {
    int n = 0;
    SDL_Locale** locs = SDL_GetPreferredLocales(&n);
    std::string out = "en";
    for (int i = 0; locs && i < n; ++i)
        if (locs[i] && kStrings.count(locs[i]->language)) { out = locs[i]->language; break; }
    SDL_free(locs);
    return out;
}

int launcher_main(int argc, char** argv) {
    (void)argc;
    App app;
    Ctx& c = app.ctx;
    const char* base = SDL_GetBasePath();
    c.exe_dir = base ? fs::u8path(base) : fs::current_path();
    c.launcher_exe = fs::u8path(argv[0]);
    if (c.launcher_exe.is_relative() || !fs::exists(c.launcher_exe)) {
        for (const char* n : {"Hercules_Recompiled.exe", "Hercules_Recompiled"})
            if (fs::exists(c.exe_dir / n)) { c.launcher_exe = c.exe_dir / n; break; }
    }
    c.store.path = c.exe_dir / "launcher.ini";
    c.store.load();
    use_release(c, app.release_for_ui());
    if (c.exe.empty() && c.id == OWN_ID) c.exe = c.launcher_exe;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "launcher: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    app.win = SDL_CreateWindow("Hercules Recompiled", 1024, 768, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    app.ren = app.win ? SDL_CreateRenderer(app.win, nullptr) : nullptr;
    if (!app.ren) {
        std::fprintf(stderr, "launcher: no window: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderVSync(app.ren, 1);

    // The launcher draws itself from a disc: the chosen release's, else any
    // the player gave. With none at all, ask for one; a disc of another
    // release than the language's switches the language to it.
    while (!app.load_disc_art()) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Hercules Recompiled", app.T("disc.first").c_str(), app.win);
        const fs::path p = pick_disc(app.win);
        if (p.empty()) { SDL_Quit(); return 0; }
        const std::string got = disc_release(p);
        if (got.empty()) {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Hercules Recompiled", disc_none_message(app, p).c_str(), app.win);
            continue;
        }
        remember_disc(c, p, got);
        if (got != c.id) app.set("language", release(got).lang);
        else c.disc = p;
    }
    app.stage = SDL_CreateTexture(app.ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET, W * SC, H * SC);
    SDL_SetTextureScaleMode(app.stage, SDL_SCALEMODE_LINEAR);
    int npads = 0;
    SDL_JoystickID* pads = SDL_GetGamepads(&npads);
    if (pads && npads > 0) app.pad = SDL_OpenGamepad(pads[0]);
    SDL_free(pads);
    app.hd = hd_read(c);
    {   // just updated: say so once
        const fs::path applied = upd_dir(c) / "applied.json";
        const std::string v = json_field(read_text(applied), "version");
        if (!v.empty()) {
            app.show_message(app.T("upd.done", {{"v", v}}));
            std::error_code ec;
            fs::remove(applied, ec);
        }
    }
    if (c.store.get("updates") != "false") app.upd_spawn("check");
    app.run();
    app.hd_stop();
    for (auto& [img, t] : app.tex) SDL_DestroyTexture(t);
    SDL_DestroyTexture(app.stage);
    SDL_DestroyRenderer(app.ren);
    SDL_DestroyWindow(app.win);
    SDL_Quit();
    return 0;
}

// ======================================================== the in-game menu
// Esc / Select+Start in the running game (psx_game_menu.h): the launcher's
// options that work live, the controls, resume and back to the launcher. The
// guest is frozen meanwhile. Drawn on the CPU into a window-sized image in
// the launcher's look (the game's font and plaster from the disc); settings
// go to launcher.ini, which the launcher reloads when the game ends.
namespace ingame {

struct Canvas {
    int w = 0, h = 0, k = 1, ox = 0, oy = 0;      // stage px -> k window px, 4:3 stage area at ox
    float sw = W;                                  // stage width incl. the sides
    std::vector<uint32_t> px;
    void resize(int ww, int wh) {
        w = std::max(ww, 320); h = std::max(wh, 240);
        k = std::max(1, h / 240);
        oy = (h - 240 * k) / 2;
        sw = float(w) / k;
        ox = (w - 320 * k) / 2;
        px.assign(size_t(w) * h, 0);
    }
    static uint32_t blend(uint32_t d, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        if (a == 255) return 0xFF000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
        const uint32_t da = d >> 24, dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
        const uint32_t oa = a + da * (255 - a) / 255;
        if (!oa) return 0;
        auto mix = [&](uint32_t s, uint32_t dd) { return (s * a + dd * da * (255 - a) / 255) / oa; };
        return (oa << 24) | (mix(r, dr) << 16) | (mix(g, dg) << 8) | mix(b, db);
    }
    // stage rect (x relative to the 4:3 area), straight alpha colour
    void fill(float x, float y, float fw, float fh, SDL_Color c) {
        const int x0 = std::max(0, ox + int(x * k)), y0 = std::max(0, oy + int(y * k));
        const int x1 = std::min(w, ox + int((x + fw) * k)), y1 = std::min(h, oy + int((y + fh) * k));
        for (int yy = y0; yy < y1; ++yy)
            for (int xx = x0; xx < x1; ++xx) px[size_t(yy) * w + xx] = blend(px[size_t(yy) * w + xx], c.r, c.g, c.b, c.a);
    }
    // src rect of an RGBA image to stage (dx, dy) at f stage px per source px
    void blit(const Image& im, int sx, int sy, int sw_, int sh, float dx, float dy, float f = 1, uint8_t alpha = 255) {
        const int x0 = ox + int(std::floor(dx * k)), y0 = oy + int(std::floor(dy * k));
        const int ow = int(sw_ * f * k + 0.5f), oh = int(sh * f * k + 0.5f);
        for (int yy = 0; yy < oh; ++yy) {
            const int ty = y0 + yy;
            if (ty < 0 || ty >= h) continue;
            const int syy = sy + std::min(sh - 1, int(yy / (f * k)));
            for (int xx = 0; xx < ow; ++xx) {
                const int tx = x0 + xx;
                if (tx < 0 || tx >= w) continue;
                const uint8_t* s = im.at(sx + std::min(sw_ - 1, int(xx / (f * k))), syy);
                const uint8_t a = uint8_t(s[3] * alpha / 255);
                if (a) px[size_t(ty) * w + tx] = blend(px[size_t(ty) * w + tx], s[0], s[1], s[2], a);
            }
        }
    }
};

struct Menu {
    bool loaded = false, failed = false, open = false, dirty = true, quit_armed = false;
    bool levels_note = false;     // HD scenery switched on: it shows from the next level
    int capture = -1;             // keys page: button waiting for its new key
    Ctx ctx;
    Font font;
    Image plaster;
    Canvas cv;
    int page = 0, row = 1;
    uint32_t pad_prev = 0;
    uint64_t pad_repeat_at = 0;
};
Menu m;

// the launcher's language: the chosen one, else the system's
std::string menu_lang() {
    const std::string l = m.ctx.store.get("language");
    if (kStrings.count(l)) return l;
    static const std::string sys = system_language();
    return sys;
}

std::string T(const std::string& key) {
    const auto& tbl = kStrings.at(menu_lang());
    auto it = tbl.find(key);
    return it == tbl.end() ? key : it->second;
}

const std::pair<int, int>& glyph(char32_t c) {
    auto it = m.font.glyphs.find(upper(c));
    return it == m.font.glyphs.end() ? m.font.glyphs.at(U' ') : it->second;
}
float text_width(const std::string& s, float f = 1) {
    f *= m.font.scale;
    int w = 0;
    for (char32_t c : utf32(s)) w += glyph(c).second + 1;
    return std::max(0, w - 1) * f;
}
void text(const std::string& s, float x, float y, const char* color, const char* align = "left", float f = 1) {
    const Image& atlas = std::strcmp(color, "blue") == 0 ? m.font.blue
                       : std::strcmp(color, "orange") == 0 ? m.font.orange : m.font.yellow;
    const float tw = text_width(s, f);
    f *= m.font.scale;
    float cx = std::strcmp(align, "center") == 0 ? x - tw / 2 : std::strcmp(align, "right") == 0 ? x - tw : x;
    for (char32_t c : utf32(s)) {
        const auto& g = glyph(c);
        m.cv.blit(atlas, g.first, 0, g.second, m.font.height, cx, y, f);
        cx += (g.second + 1) * f;
    }
}

// ------------------------------------------------------------------ pages
struct MRow { std::string key; std::vector<std::pair<std::string, std::string>> choices; };
struct MPage { std::string title; std::vector<MRow> rows; };

// The PlayStation buttons Hercules uses; L3/R3 and the sticks stay as they are.
const int kRebind[] = {PSX_KB_UP, PSX_KB_DOWN, PSX_KB_LEFT, PSX_KB_RIGHT, PSX_KB_CROSS, PSX_KB_CIRCLE,
                       PSX_KB_SQUARE, PSX_KB_TRIANGLE, PSX_KB_L1, PSX_KB_R1, PSX_KB_L2, PSX_KB_R2,
                       PSX_KB_START, PSX_KB_SELECT};

std::string key_label(int button) {
    const SDL_Scancode sc = psx_keybinds_get_button(1, button);
    if (sc == SDL_SCANCODE_UNKNOWN) return "-";
    std::string n = SDL_GetScancodeName(sc);
    for (char& ch : n) ch = char(std::toupper((unsigned char)ch));
    return n.empty() ? "?" : n;
}

// A new key for one button. The framework frees the key from any other
// button, so it cannot also stay on the left stick (by default the arrow keys
// drive both); Hercules reads only the digital pad, so a rebound direction
// clears the stick's key instead of leaving the old one there.
void rebind(int button, SDL_Scancode sc) {
    psx_keybinds_set_button(1, button, sc);
    const int stick = button == PSX_KB_UP ? PSX_KB_LS_UP : button == PSX_KB_DOWN ? PSX_KB_LS_DOWN
                    : button == PSX_KB_LEFT ? PSX_KB_LS_LEFT : button == PSX_KB_RIGHT ? PSX_KB_LS_RIGHT : -1;
    if (stick >= 0) {
        psx_keybinds_set_button(1, stick, SDL_SCANCODE_UNKNOWN);
        psx_keybinds_set_button_alt(1, stick, SDL_SCANCODE_UNKNOWN);
    }
    psx_keybinds_save();
}

std::vector<MPage> menu_pages() {
    auto from_launcher = [](const char* key) {
        for (const auto& [name, rows] : pages())
            for (const auto& r : rows) if (r.key == key) return MRow{r.key, r.choices};
        return MRow{key, kOnOff};
    };
    std::vector<MPage> out;
    out.push_back({"pause", {{"act.resume", {}}, {"act.quit", {}}}});
    MPage gfx{"gfx", {}};
    if (hd_pack_ready(m.ctx))
        for (const char* k : {"hd_sprites", "hd_levels", "hd_hud"}) gfx.rows.push_back(from_launcher(k));
    for (const char* k : {"texture_filtering", "aspect_ratio"}) gfx.rows.push_back(from_launcher(k));
    gfx.rows.push_back({"fullscreen", {{"0", "window"}, {"1", "full"}}});
    for (const char* k : {"perspective_texturing", "geometry_correction"}) gfx.rows.push_back(from_launcher(k));
    out.push_back(gfx);
    MPage filter{"filter", {}};
    for (const char* k : {"crt_sim", "crt_mask", "crt_bloom", "scanlines", "scanline_strength", "crt_filter"})
        filter.rows.push_back(from_launcher(k));
    out.push_back(filter);
    MRow vol{"volume", {}};
    for (int v = 0; v <= 100; v += 10) vol.choices.push_back({std::to_string(v), "=" + std::to_string(v) + " %"});
    out.push_back({"sound", {vol}});
    MPage keys{"keys", {}};                      // player 1's keyboard keys, live (keybinds.ini)
    for (int b : kRebind) keys.rows.push_back({std::string("kb.") + psx_keybinds_button_name(b), {}});
    keys.rows.push_back({"act.kb_reset", {}});
    out.push_back(keys);
    out.push_back({"hotkeys", {}});              // what cannot be changed here
    return out;
}

std::string value_of(const std::string& key) {
    if (key == "volume") {
        const std::string v = m.ctx.store.get("volume");
        return v.empty() ? std::to_string(psx_host_get_volume()) : v;
    }
    if (key == "fullscreen") return psx_host_get_fullscreen() ? "1" : "0";
    return m.ctx.store.get(key);
}
std::string label_of(const MRow& r) {
    if (r.choices.empty()) return "";
    const std::string l = r.choices[choice_index(value_of(r.key), r.choices)].second;
    if (!l.empty() && l[0] == '=') return l.substr(1);
    if (l == "on" || l == "off") return T(l);
    return T("val." + l);
}

// ------------------------------------------------------------ apply live
float num(const std::string& k, float def) {
    const std::string v = m.ctx.store.get(k);
    return v.empty() ? def : float(std::atof(v.c_str()));
}
void apply(const std::string& key) {
    const auto& st = m.ctx.store;
    if (key == "hd_sprites" || key == "hd_levels" || key == "hd_hud") {
        if (key == "hd_levels") m.levels_note = st.b("hd_levels");
        const fs::path pack = pack_for(m.ctx);
        psx_host_set_hd_pack(pack.empty() ? "" : pack.u8string().c_str());
    } else if (key == "texture_filtering") {
        const std::string v = st.get(key);
        psx_host_set_texture_filter(v == "bilinear" ? 1 : v == "xbr" ? 3 : 0);
    } else if (key == "aspect_ratio") {
        if (st.get(key) == "16:9") psx_host_set_display_aspect(16, 9);
        else psx_host_set_display_aspect(4, 3);
    } else if (key == "fullscreen") {
        psx_host_set_fullscreen(st.get(key) == "0" ? 0 : 1);
    } else if (key == "perspective_texturing") {
        psx_host_set_perspective(st.b(key));
    } else if (key == "geometry_correction") {
        psx_host_set_geometry(st.b(key));
    } else if (key == "crt_sim" || key == "crt_mask" || key == "crt_bloom" || key == "scanlines" ||
               key == "scanline_strength") {
        psx_host_set_crt(st.b("crt_sim"), num("crt_mask", 0.5f), num("crt_bloom", 0.35f));
        psx_host_set_scanlines(st.b("scanlines") || st.b("crt_sim"), num("scanline_strength", 0.35f));
    } else if (key == "crt_filter") {
        const std::string v = st.get(key);
        psx_host_set_screen_kind(v == "crt" ? 1 : v == "composite" ? 2 : v == "trinitron" ? 3 : 0);
    } else if (key == "volume") {
        psx_host_set_volume(std::atoi(st.get(key).c_str()));
    }
}

// --------------------------------------------------------------- drawing
void render() {
    int ww = 0, wh = 0;
    psx_host_window_size(&ww, &wh);
    if (ww <= 0 || wh <= 0) { ww = 1280; wh = 960; }
    m.cv.resize(ww, wh);
    const float left = -float(m.cv.ox) / m.cv.k;
    m.cv.fill(left, -float(m.cv.oy) / m.cv.k, m.cv.sw, float(m.cv.h) / m.cv.k, {8, 3, 2, 170});   // dim the game
    if (!m.plaster.empty()) m.cv.blit(m.plaster, 0, 0, m.plaster.w, std::min(m.plaster.h, 240), 0, 0, 1, 238);
    const auto ps = menu_pages();
    const MPage& pg = ps[size_t(m.page) % ps.size()];
    text("< " + T("pages." + pg.title) + " >", W / 2.0f, 6, m.row == 0 ? "blue" : "yellow", "center");
    if (pg.title == "keys") {
        for (size_t i = 0; i < pg.rows.size(); ++i) {
            const MRow& r = pg.rows[i];
            const bool active = m.row == int(i) + 1;
            const float y = 28 + i * 11.6f;
            if (i < std::size(kRebind)) {
                text(T(r.key), 14, y, active ? "blue" : "yellow", "left", 0.6f);
                const bool waiting = m.capture == kRebind[i];
                text(waiting ? T("kb.press") : key_label(kRebind[i]), 306, y,
                     waiting ? "blue" : active ? "blue" : "orange", "right", 0.6f);
            } else {
                text(T("kb.reset"), W / 2.0f, y + 4, active ? "blue" : "yellow", "center", 0.6f);
            }
        }
    } else if (pg.title == "hotkeys") {
        const std::string l = menu_lang();
        for (int i = 5; i < 20; ++i) {               // 0..4 are the game buttons (page TASTEN)
            auto it = kStrings.at(l).find("ingame.keys." + std::to_string(i));
            if (it == kStrings.at(l).end()) break;
            const std::string& line = it->second;
            const size_t bar = line.find('|');
            const float y = 32 + (i - 5) * 16.0f;
            text(line.substr(0, bar), 14, y, "orange", "left", 0.6f);
            if (bar != std::string::npos) text(line.substr(bar + 1), 108, y, "yellow", "left", 0.6f);
        }
    } else {
        for (size_t i = 0; i < pg.rows.size(); ++i) {
            const MRow& r = pg.rows[i];
            const bool active = m.row == int(i) + 1;
            const float y = 38 + i * 21;
            if (r.choices.empty()) {             // an action
                std::string label = T("ingame." + r.key.substr(4));
                if (r.key == "act.quit" && m.quit_armed) label = T("ingame.quit_sure");
                text(label, W / 2.0f, 40 + i * 30, active ? "blue" : "yellow", "center");
            } else {
                text(T("opt." + r.key), 14, y, active ? "blue" : "yellow", "left", 0.85f);
                text(label_of(r), 306, y, active ? "blue" : "orange", "right", 0.85f);
            }
        }
        if (pg.title == "pause") text(T("ingame.hint"), W / 2.0f, 150, "orange", "center", 0.6f);
        // level textures are matched when a level uploads them, so switching
        // HD scenery back on shows from the next level (BUG-20261014-004)
        if (pg.title == "gfx" && m.levels_note)
            text(T("ingame.hd_levels_next"), W / 2.0f, 203, "orange", "center", 0.6f);
    }
    text(T("help_back"), W / 2.0f, 218, "yellow", "center", 0.7f);
    m.dirty = false;
}

// ---------------------------------------------------------------- input
void close_menu() { m.open = false; m.quit_armed = false; m.levels_note = false; m.capture = -1; }

void step(int d) {
    const auto ps = menu_pages();
    const int np = int(ps.size());
    if (m.row == 0) { m.page = (m.page + d + np) % np; m.quit_armed = false; m.dirty = true; return; }
    const MPage& pg = ps[size_t(m.page)];
    if (m.row - 1 >= int(pg.rows.size())) return;
    const MRow& r = pg.rows[size_t(m.row - 1)];
    if (r.choices.empty()) {                     // actions: on OK only
        if (d != 0) return;
        if (r.key == "act.resume") close_menu();
        else if (r.key == "act.kb_reset") { psx_keybinds_reset_player(1); psx_keybinds_save(); m.dirty = true; }
        else if (r.key.rfind("kb.", 0) == 0) { m.capture = kRebind[m.row - 1]; m.dirty = true; }
        else if (r.key == "act.quit") {
            if (!m.quit_armed) { m.quit_armed = true; m.dirty = true; return; }
            m.ctx.store.save();
            psx_host_quit();
        }
        return;
    }
    const int n = int(r.choices.size());
    const int dd = d == 0 ? 1 : d;
    const std::string nv = r.choices[size_t((choice_index(value_of(r.key), r.choices) + dd + n) % n)].first;
    m.ctx.store.kv[r.key] = nv;
    m.ctx.store.save();
    apply(r.key);
    m.dirty = true;
}

void nav(Key k) {
    const auto ps = menu_pages();
    const MPage& pg = ps[size_t(m.page) % ps.size()];
    const int n = int(pg.rows.size()) + 1;
    if (k != Key::Ok) m.quit_armed = false;
    switch (k) {
    case Key::Up: m.row = (m.row + n - 1) % n; break;
    case Key::Down: m.row = (m.row + 1) % n; break;
    case Key::Left: step(-1); break;
    case Key::Right: step(1); break;
    case Key::Ok: if (m.row == 0) step(1); else step(0); break;
    case Key::Back: close_menu(); break;
    default: return;
    }
    m.dirty = true;
}

// ---------------------------------------------------------------- hooks
bool load() {
    if (m.loaded) return !m.failed;
    m.loaded = true;
    Ctx& c = m.ctx;
    const char* base = SDL_GetBasePath();
    const char* home = SDL_getenv("PSX_HL_HOME");     // a build in lang/<id>/, started by the launcher
    c.exe_dir = home && *home ? fs::u8path(home) : base ? fs::u8path(base) : fs::current_path();
    c.store.path = c.exe_dir / "launcher.ini";
    c.store.load();
    use_release(c, OWN_ID);
    Disc d;
    std::vector<uint8_t> b;
    std::string err;
    // the font as in the launcher: the German disc's strip if there is one
    const fs::path de = find_disc(c, "SCES-00893");
    const fs::path font_disc = de.empty() ? c.disc : de;
    const std::string font_id = de.empty() ? c.id : "SCES-00893";
    Disc fd;
    if (font_disc.empty() || !fd.open(font_disc, &err) || !fd.read("STP/STP1.", b) ||
        !build_font(b, m.font, &err, font_id) || c.disc.empty() || !d.open(c.disc, &err)) {
        std::fprintf(stderr, "hercules menu: no disc art (%s)\n", err.c_str());
        m.failed = true;
        return false;
    }
    std::fprintf(stderr, "hercules menu: %s, HD pack %s\n", c.exe_dir.u8string().c_str(),
                 hd_pack_ready(c) ? "found" : "not found");
    if (d.read("TI/T03.0", b)) {
        auto ims = decode_fullscreen(b);
        if (!ims.empty()) m.plaster = std::move(ims[0]);
    }
    return true;
}

void hook_open() {
    if (!load()) { psx_host_osd("Menu unavailable: disc art not found", 2500); return; }
    m.ctx.store.load();
    m.open = true;
    m.page = 0;
    m.row = 1;
    m.quit_armed = false;
    m.pad_prev = psx_host_p1_buttons();          // the opening chord is not a press
    m.dirty = true;
}
int hook_is_open() { return m.open ? 1 : 0; }
void hook_key(int key, int scancode, int, int repeat) {
    if (m.capture >= 0) {                        // waiting for a new key
        if (repeat) return;
        const SDL_Scancode sc = SDL_Scancode(scancode);
        const bool host = (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) || sc == SDL_SCANCODE_TAB;
        if (key != SDLK_ESCAPE && !host && sc != SDL_SCANCODE_UNKNOWN) rebind(m.capture, sc);
        m.capture = -1;                          // Esc (or a host key) cancels
        m.dirty = true;
        return;
    }
    switch (key) {
    case SDLK_UP: nav(Key::Up); break;
    case SDLK_DOWN: nav(Key::Down); break;
    case SDLK_LEFT: nav(Key::Left); break;
    case SDLK_RIGHT: nav(Key::Right); break;
    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: case SDLK_X: if (!repeat) nav(Key::Ok); break;
    case SDLK_ESCAPE: case SDLK_BACKSPACE: if (!repeat) nav(Key::Back); break;
    default: break;
    }
}
void hook_tick(uint32_t now) {
    const uint32_t b = psx_host_p1_buttons();
    auto bit = [](int code) { return 1u << code; };
    const uint32_t pressed = b & ~m.pad_prev;
    const uint32_t dirs = bit(SDL_GAMEPAD_BUTTON_DPAD_UP) | bit(SDL_GAMEPAD_BUTTON_DPAD_DOWN) |
                          bit(SDL_GAMEPAD_BUTTON_DPAD_LEFT) | bit(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    uint32_t fire = pressed;
    if (b & dirs) {                              // held direction: repeat
        if (pressed & dirs) m.pad_repeat_at = now + 350;
        else if (now >= m.pad_repeat_at) { fire |= b & dirs; m.pad_repeat_at = now + 110; }
    }
    m.pad_prev = b;
    if (m.capture >= 0) {                        // keys are rebound from the keyboard only
        if (pressed) { m.capture = -1; m.dirty = true; }
        return;
    }
    if (fire & bit(SDL_GAMEPAD_BUTTON_DPAD_UP)) nav(Key::Up);
    if (fire & bit(SDL_GAMEPAD_BUTTON_DPAD_DOWN)) nav(Key::Down);
    if (fire & bit(SDL_GAMEPAD_BUTTON_DPAD_LEFT)) nav(Key::Left);
    if (fire & bit(SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) nav(Key::Right);
    if (pressed & bit(SDL_GAMEPAD_BUTTON_SOUTH)) nav(Key::Ok);
    if (pressed & (bit(SDL_GAMEPAD_BUTTON_EAST) | bit(SDL_GAMEPAD_BUTTON_NORTH) | bit(SDL_GAMEPAD_BUTTON_START)))
        nav(Key::Back);
}
int hook_image(const uint32_t** px, int* w, int* h) {
    if (!m.open) return 0;
    int ww = 0, wh = 0;
    psx_host_window_size(&ww, &wh);
    if (m.dirty || std::max(ww, 320) != m.cv.w || std::max(wh, 240) != m.cv.h) render();
    *px = m.cv.px.data();
    *w = m.cv.w;
    *h = m.cv.h;
    return 1;
}

const PsxGameMenuHooks kHooks = {hook_open, hook_is_open, hook_key, hook_tick, hook_image};

}  // namespace ingame

struct Register {
    Register() {
        psx_game_launcher_register(&launcher_main);
        psx_game_menu_register(&ingame::kHooks);
    }
} g_register;

}  // namespace
}  // namespace hl
