//
// sysres_gen.cpp - Port of sysres_gen.py
// Builds font + keymap SYSROM tables from sysres/font_std.png + charmap.txt
//
// Uses stb_image (header-only) so no libpng / SDL_image dependency is required.
//

#include "sysres_gen.h"
#include "font_std.h"
#include "charmap.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_THREAD_LOCALS
#include "extern/stb_image.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <algorithm>
#include <cctype>

// ---------------------------------------------------------------------------
// Font table: 256 tiles x 8 rows -> 2048 words
// ---------------------------------------------------------------------------
std::vector<uint16_t> buildFontWords(const std::string& pngPath) {
    std::vector<uint16_t> words(256 * TILE_SIZE, 0);

    int width = 0, height = 0, channels = 0;
    // Request 1 channel; stb converts RGB/grey/palette to greyscale for us.
    unsigned char* data = stbi_load(pngPath.c_str(), &width, &height, &channels, 1);
    if (!data) {
        std::cerr << "sysres: failed to load PNG '" << pngPath << "': "
                  << stbi_failure_reason() << "\n";
        return words;
    }

    const int tiles_per_row = width / TILE_SIZE;
    if (tiles_per_row <= 0) {
        std::cerr << "sysres: PNG width " << width
                  << " is smaller than TILE_SIZE " << TILE_SIZE << "\n";
        stbi_image_free(data);
        return words;
    }

    for (int tile = 0; tile < 256; ++tile) {
        const int tx = (tile % tiles_per_row) * TILE_SIZE;
        const int ty = (tile / tiles_per_row) * TILE_SIZE;
        if (ty >= height) break;

        for (int row = 0; row < TILE_SIZE; ++row) {
            uint16_t rowbits = 0;
            for (int col = 0; col < TILE_SIZE; ++col) {
                const int px = tx + col;
                const int py = ty + row;
                if (px >= width || py >= height) continue;
                // Any non-zero sample = "on" (matches PIL mode "1")
                if (data[py * width + px]) {
                    rowbits = static_cast<uint16_t>(rowbits | (1 << (7 - col)));
                }
            }

            // Sequential tile-major: tile N occupies words N*8 .. N*8+7
            // (matches put_tile: font_addr = tile_idx * 8 + row)
            words[tile * TILE_SIZE + row] = rowbits;
        }
    }

    stbi_image_free(data);
    return words;
}

// ---------------------------------------------------------------------------
// charmap.txt parser: name -> tile index
// Order matters: build_keymap_words() below relies on file order the same
// way Python's insertion-ordered dict does (A-Z appearing after a-z means
// the uppercase tile wins when both cases mirror onto the same ASCII slot).
// An unordered_map here would silently randomize that per letter.
//
// Takes the already-read text content directly so both the file-based path
// (chars.txt on disk) and the built-in path (charmap.h, baked into the
// binary) can share this same parsing logic.
//
// Format is simply: <name> <tile_index>
// Single-character names (ASCII printable + space) feed the 128-entry
// keymap; multi-character names (icons, named symbols) are ignored by the
// keymap builder and exist only for documentation / future direct-tile use.
// ---------------------------------------------------------------------------
static std::vector<std::pair<std::string, int>> parseCharsContent(std::istream& in) {
    std::vector<std::pair<std::string, int>> mapping;

    std::string line;
    while (std::getline(in, line)) {
        auto notSpace = [](unsigned char c) { return !std::isspace(c); };
        line.erase(line.begin(), std::ranges::find_if(line, notSpace));
        line.erase(std::find_if(line.rbegin(), line.rend(), notSpace).base(), line.end());
        if (line.empty()) continue;

        std::istringstream ls(line);
        std::string name, idxStr;
        if (!(ls >> name >> idxStr)) continue;

        try {
            int tile = std::stoi(idxStr);
            mapping.emplace_back(name, tile);
        } catch (...) {
        }
    }

    // "space" -> " " alias, appended so it still comes last (matches Python
    // setting mapping[" "] once, right after the initial parse).
    for (size_t i = 0; i < mapping.size(); ++i) {
        if (mapping[i].first == "space") {
            mapping.emplace_back(" ", mapping[i].second);
            break;
        }
    }
    return mapping;
}

static std::vector<std::pair<std::string, int>> parseCharsTxt(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        std::cerr << "sysres: cannot open " << path << "\n";
        return {};
    }
    return parseCharsContent(in);
}

static std::vector<uint16_t> keymapFromMapping(
    const std::vector<std::pair<std::string, int>>& mapping) {
    std::vector<uint16_t> table(KEYMAP_SIZE, NO_GLYPH);

    // Step 1: Map all explicit definitions exactly as they are defined in charmap.txt
    for (const auto& [name, tile] : mapping) {
        if (name.size() == 1) {
            const unsigned char code = static_cast<unsigned char>(name[0]);
            if (code < KEYMAP_SIZE) {
                table[code] = static_cast<uint16_t>(tile);
            }
        }
    }

    // Step 2: Only apply a case fallback if the destination slot is completely empty (NO_GLYPH)
    for (const auto& [name, tile] : mapping) {
        if (name.size() == 1) {
            if (name[0] >= 'a' && name[0] <= 'z') {
                unsigned char upper = static_cast<unsigned char>(std::toupper(name[0]));
                if (table[upper] == NO_GLYPH) {
                    table[upper] = static_cast<uint16_t>(tile);
                }
            } else if (name[0] >= 'A' && name[0] <= 'Z') {
                unsigned char lower = static_cast<unsigned char>(std::tolower(name[0]));
                if (table[lower] == NO_GLYPH) {
                    table[lower] = static_cast<uint16_t>(tile);
                }
            }
        }
    }
    return table;
}

std::vector<uint16_t> buildKeymapWords(const std::string& txtPath) {
    return keymapFromMapping(parseCharsTxt(txtPath));
}

// ---------------------------------------------------------------------------
// Built-in variants - source data from font_std.h / charmap.h instead of
// reading files. font_std[] is already exactly what buildFontWords() would
// have computed from a PNG (one packed row-bits byte per row, 256 tiles x 8
// rows), so this is a straight copy rather than a re-derivation.
// ---------------------------------------------------------------------------
std::vector<uint16_t> buildFontWordsBuiltin() {
    static_assert(sizeof(font_std) / sizeof(font_std[0]) == 256 * TILE_SIZE,
                  "font_std.h size drifted from 256 tiles x TILE_SIZE rows");
    std::vector<uint16_t> words(256 * TILE_SIZE);
    for (size_t i = 0; i < words.size(); ++i) {
        words[i] = static_cast<uint16_t>(font_std[i] & 0xFF);
    }
    return words;
}

std::vector<uint16_t> buildKeymapWordsBuiltin() {
    // charmap[] is the exact byte content of charmap.txt (not
    // null-terminated), so wrap it in an istringstream the same way
    // parseCharsTxt wraps an ifstream.
    std::string content;
    content.reserve(sizeof(charmap) / sizeof(charmap[0]));
    for (size_t i = 0; i < sizeof(charmap) / sizeof(charmap[0]); ++i) {
        content.push_back(static_cast<char>(charmap[i] & 0xFF));
    }
    std::istringstream in(content);
    return keymapFromMapping(parseCharsContent(in));
}

bool loadSysresBuiltin(Memory& mem) {
    auto font = buildFontWordsBuiltin();
    auto keymap = buildKeymapWordsBuiltin();

    std::vector<uint16_t> combined;
    combined.reserve(font.size() + keymap.size());
    combined.insert(combined.end(), font.begin(), font.end());
    combined.insert(combined.end(), keymap.begin(), keymap.end());

    if (!mem.loadSysrom(combined)) {
        std::cerr << "sysres: loadSysrom failed\n";
        return false;
    }

    std::cout << "sysres: loaded built-in font (" << font.size()
              << " words) + keymap (" << keymap.size()
              << " words) into SYSROM\n";
    return true;
}


// ---------------------------------------------------------------------------
// Public entry: burn both tables into SYSROM
// ---------------------------------------------------------------------------
bool loadSysres(Memory& mem,
                const std::string& charsPng,
                const std::string& charsTxt) {
    auto font = buildFontWords(charsPng);
    auto keymap = buildKeymapWords(charsTxt);

    if (static_cast<int>(font.size()) != KEYMAP_BASE) {
        std::cerr << "sysres: font table size " << font.size()
                  << " drifted from KEYMAP_BASE " << KEYMAP_BASE << "\n";
        return false;
    }

    std::vector<uint16_t> combined;
    combined.reserve(font.size() + keymap.size());
    combined.insert(combined.end(), font.begin(), font.end());
    combined.insert(combined.end(), keymap.begin(), keymap.end());

    if (!mem.loadSysrom(combined)) {
        std::cerr << "sysres: loadSysrom failed\n";
        return false;
    }

    std::cout << "sysres: loaded font (" << font.size()
              << " words) + keymap (" << keymap.size()
              << " words) into SYSROM\n";
    return true;
}