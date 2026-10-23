// Hercules launcher: everything it shows, read from the player's own disc.
//
// C++ port of the decomp tools the Python launcher used (iso_extract.py,
// fullscreen2png.py, sprites.py, build_assets.py font()): nothing of the game
// ships with the executable; the art is decoded at start-up from the disc.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace PS1 { class ISOReader; }

namespace hl {

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;          // w*h*4, straight alpha
    int ax = 0, ay = 0;                 // a sprite frame's anchor (where the game places it) in the image
    bool empty() const { return w <= 0 || h <= 0; }
    uint8_t* at(int x, int y) { return &rgba[(size_t(y) * w + x) * 4]; }
    const uint8_t* at(int x, int y) const { return &rgba[(size_t(y) * w + x) * 4]; }
    static Image make(int w, int h) { Image i; i.w = w; i.h = h; i.rgba.assign(size_t(w) * h * 4, 0); return i; }
    Image crop(int x0, int y0, int x1, int y1) const;
    Image crop_alpha() const;           // to the opaque bounding box
};

// ISO 9660 of a disc image, read through the runtime's own disc code
// (PSXRecompV4::resolve_disc_path + PS1::ISOReader), so the launcher accepts
// exactly what the game mounts: .cue (quoted or bare FILE, renamed or
// differently cased .bin), .bin/.img, .iso and .chd.
class Disc {
public:
    bool open(const std::filesystem::path& cue_or_bin, std::string* error);
    // "EX/TITLE." style path (as on the disc, without ";1"); Form 1 data
    bool read(const std::string& path, std::vector<uint8_t>& out) const;
    std::vector<std::string> list(const std::string& dir) const;   // file names in a folder
private:
    struct Entry { uint32_t lba, size; };
    bool sector(uint32_t lba, uint8_t* out2048) const;
    void walk(uint32_t lba, uint32_t size, const std::string& prefix, int depth);
    std::shared_ptr<PS1::ISOReader> iso_;
    std::map<std::string, Entry> files_;
};

// Full-screen pictures (TI/, CR/): 18-byte header + 15-bit pixels, several per file.
std::vector<Image> decode_fullscreen(const std::vector<uint8_t>& blob);

// A sprite file (SD*/S*.N, tag i1.4): the unique frames in the order the
// decomp's sprites.py writes them (frame number -> image).
struct SpriteFrames {
    std::map<int, Image> frames;        // key: 1-based frame number (fNNNN)
    std::vector<int> order() const;     // sorted frame numbers
};
// dist_table: the ENGINE1 address of the run-length table, which moved when
// each PAL release was relinked (Release::dist_table in hl_launcher.cpp).
bool decode_sprites(const std::vector<uint8_t>& data, const std::vector<uint8_t>& engine1, uint32_t dist_table,
                    SpriteFrames& out, std::string* error);

// The pause-menu font from the HUD strip (STP/STP1): glyph atlases in the
// game's yellow and orange plus a blue recolour, one row of glyphs.
struct Font {
    int height = 0, baseline = 0;
    // drawn at this size: the German pause font's capitals (13 px) set every
    // layout; the English and Italian ones are 18 px, French and Spanish 15
    float scale = 1;
    std::map<char32_t, std::pair<int, int>> glyphs;   // char -> (x, width) in the atlas
    Image yellow, orange, blue;
};
// release: the disc's PAL release (SCES-0089x); each holds other letters.
bool build_font(const std::vector<uint8_t>& stp, Font& out, std::string* error,
                const std::string& release = "SCES-00893");

}  // namespace hl
