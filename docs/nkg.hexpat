#pragma author NuevoAuto
#pragma description NuevoAuto packed disk image (.nkg)
#pragma MIME application/x-nuevoauto-disk
#pragma magic [ 4B 4E ] @ 0x00
#pragma endian little

// NuevoAuto disk image (.nkg)
// Layout mirrors src/memory.h and tools/mkdisk.py
//
// All multi-byte fields are little-endian uint16 words.
// Directory offsets/lengths are absolute WORD indices into the image
// (byte offset = word_index * 2).
//
// Magic 0x4E4B ("NK"), version 1
// Header: 5 words
// Directory: 128 entries × 16 words starting at word 5
// Data area: from word 2053 onward

import std.mem;
import std.string;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
#define DISK_MAGIC        0x4E4B
#define DISK_VERSION      1
#define DISK_MAX_FILES    128
#define DISK_NAME_WORDS   8

// ---------------------------------------------------------------------------
// Enums / flags
// ---------------------------------------------------------------------------
enum FileType : u16 {
    Program  = 0,
    Graphics = 1,
    Font     = 2,
    Data     = 3,
    Other    = 4
};

bitfield FileFlags {
    bootable : 1;   // 0x0001
    readonly : 1;   // 0x0002
    padding  : 14;
};

// ---------------------------------------------------------------------------
// Format helpers (take the whole struct / scalar — no array params)
// ---------------------------------------------------------------------------
fn combine_hi_lo(u16 hi, u16 lo) {
    return (u32(hi) << 16) | u32(lo);
};

fn format_magic(u16 v) {
    if (v == DISK_MAGIC)
        return "NK (0x4E4B)";
    return std::format("BAD {:#06x}", v);
};

fn format_entry(auto e) {
    // Free slot: all name words zero
    bool free = true;
    for (u8 i = 0, i < DISK_NAME_WORDS, i = i + 1) {
        if (e.name[i] != 0) {
            free = false;
            break;
        }
    }
    if (free)
        return "(free)";

    // Decode packed name (low byte of each word, stop at 0 or space)
    str name = "";
    for (u8 i = 0, i < DISK_NAME_WORDS, i = i + 1) {
        u16 w = e.name[i];
        u8 ch = u8(w & 0xFF);
        if (w == 0x0000 || ch == 0x20)
            break;
        name = name + char(ch);
    }

    u32 off = combine_hi_lo(e.offset_hi, e.offset_lo);
    u32 len = combine_hi_lo(e.length_hi, e.length_lo);

    str flags = "";
    if (e.flags.bootable)
        flags = flags + " boot";
    if (e.flags.readonly)
        flags = flags + " ro";

    return std::format("{}  type={}  words@{}  len={}{}",
                       name, u16(e.type), off, len, flags);
};

// ---------------------------------------------------------------------------
// Structures
// ---------------------------------------------------------------------------
struct DiskEntry {
    u16 name[DISK_NAME_WORDS];
    FileType type;
    FileFlags flags;
    u16 offset_hi;
    u16 offset_lo;
    u16 length_hi;
    u16 length_lo;
    u16 reserved[2];
} [[format("format_entry")]];

struct DiskHeader {
    u16 magic [[format("format_magic"), color("FFD700")]];
    u16 version;
    u16 file_count;
    u16 free_offset_hi;
    u16 free_offset_lo;

    if (magic != DISK_MAGIC)
        std::warning("Bad magic: expected 0x4E4B (\"NK\")");
    if (version != DISK_VERSION)
        std::warning(std::format("Unexpected version {} (expected {})", version, DISK_VERSION));
};

// ---------------------------------------------------------------------------
// Top-level image
// ---------------------------------------------------------------------------
struct NKG {
    DiskHeader header;

    // Fixed directory table: 128 slots × 16 words (word indices 5 .. 2052)
    DiskEntry directory[DISK_MAX_FILES];

    // Packed file payloads from word 2053 to EOF
    u8 data_area[while(!std::mem::eof())] [[color("2E8B57"), name("data area")]];
};

NKG disk @ 0x00;

// ---------------------------------------------------------------------------
// Notes
// ---------------------------------------------------------------------------
// Directory entry fields:
//   name[8]       packed chars (low byte of each word), zero-terminated
//   type          0=Program 1=Graphics 2=Font 3=Data 4=Other
//   flags         bit0=bootable  bit1=readonly
//   offset_hi/lo  absolute word index of file data
//   length_hi/lo  length in words
//
// Free slots have an all-zero name.
// free_offset is where the next SAVE appends (word index).
// DISK_DATA_START is always 2053 (header 5 + 128×16 directory).
//
// To inspect a file's bytes in ImHex:
//   byte_offset = data_offset * 2
//   byte_length = data_length * 2