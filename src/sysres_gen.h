//
// sysres_gen.h - Build SYSROM font + ASCII keymap tables (port of sysres_gen.py)
//
// SYSROM layout:
//   words 0..2047    : font table, 256 tiles x 8 rows/tile, 1 word/row
//                      (bit7..bit0 = leftmost..rightmost pixel of that row)
//   words 2048..2175 : ASCII->tile keymap, one word per ASCII code 0..127
//                      (0xFFFF = no printable glyph for that key)
//

#ifndef NUEVOAUTO_SYSRES_GEN_H
#define NUEVOAUTO_SYSRES_GEN_H

#include "memory.h"
#include <string>
#include <vector>
#include <cstdint>

constexpr int TILE_SIZE = 8;
constexpr int FONT_BASE = 0;
constexpr int KEYMAP_BASE = 2048;
constexpr int KEYMAP_SIZE = 128;
constexpr uint16_t NO_GLYPH = 0xFFFF;

constexpr const char* DEFAULT_CHARS_PNG = "emures/font_std.png";
constexpr const char* DEFAULT_CHARS_TXT = "emures/charmap.txt";

// Build the 2048-word font table from a 1-bit (or greyscale) PNG of 8x8 tiles.
std::vector<uint16_t> buildFontWords(const std::string& pngPath = DEFAULT_CHARS_PNG);

// Build the 128-word ASCII keymap from charmap.txt (name -> tile index).
std::vector<uint16_t> buildKeymapWords(const std::string& txtPath = DEFAULT_CHARS_TXT);

// Built-in, immutable variants - source their data from font_std.h /
// charmap.h (compiled directly into the binary) instead of reading files
// at runtime. No PNG decoding involved: font_std[] is already the packed
// per-row bits, one byte per row, so this is just a direct copy.
std::vector<uint16_t> buildFontWordsBuiltin();
std::vector<uint16_t> buildKeymapWordsBuiltin();

// Burns the built-in font + keymap into SYSROM. This is the default boot
// path now - no font_std.png/charmap.txt file needed at all.
bool loadSysresBuiltin(Memory& mem);

// Build both tables and burn them into SYSROM (bypasses WRITEABLE).
// Returns true on success. Safe to call immediately after Memory construction.
bool loadSysres(Memory& mem,
                const std::string& charsPng = DEFAULT_CHARS_PNG,
                const std::string& charsTxt = DEFAULT_CHARS_TXT);

#endif // NUEVOAUTO_SYSRES_GEN_H