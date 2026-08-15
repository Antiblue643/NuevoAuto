//
// Created by rwarr on 8/6/2026.
//

#ifndef NUEVOAUTO_MEMORY_H
#define NUEVOAUTO_MEMORY_H
#pragma once

#include <array>
#include <vector>
#include <map>
#include <cstdint>
#include <string>
#include <string_view>

// Keys must match the strings used in Memory::Memory() when filling mem_regions.
inline std::array<std::string_view, 4> WRITEABLE = {
    "vram",
    "volatile",
    "nprog",
    "disk"
};
inline std::array<std::string_view, 5> READABLE = {
    "vram",
    "volatile",
    "nprog",
    "disk",
    "sysrom"
};

// ---------------------------------------------------------------------------
// Disk filesystem layout (lives inside the `disk` region - see memory.cpp
// for the full field-by-field description). Mirrored in tools/mkdisk.py -
// keep both in sync if these ever change.
// ---------------------------------------------------------------------------
constexpr uint16_t DISK_MAGIC        = 0x4E4B; // "NK"
constexpr uint16_t DISK_VERSION      = 1;
constexpr size_t   DISK_HEADER_WORDS = 5;      // magic, version, fileCount, nextFreeOffset(hi,lo)
constexpr size_t   DISK_MAX_FILES    = 128;
constexpr size_t   DISK_NAME_WORDS   = 8;
constexpr size_t   DISK_ENTRY_WORDS  = 16;     // name(8) type flags offHi offLo lenHi lenLo x2 reserved
constexpr size_t   DISK_DIR_START    = DISK_HEADER_WORDS;
constexpr size_t   DISK_DATA_START   = DISK_DIR_START + DISK_MAX_FILES * DISK_ENTRY_WORDS;

constexpr uint16_t DISK_FLAG_BOOTABLE = 0x0001;
constexpr uint16_t DISK_FLAG_READONLY = 0x0002;

struct DiskEntry {
    int index = -1;          // slot index in the directory table, -1 if not found
    std::string name;        // trimmed at the first zero/space word, up to 8 chars
    uint16_t type = 0;
    uint16_t flags = 0;
    uint32_t offset = 0;     // absolute word offset into the `disk` vector
    uint32_t length = 0;     // length in words
};

class Memory {
public:
    Memory();

    std::vector<uint16_t> vram;
    std::vector<uint16_t> volatile_mem;
    std::vector<uint16_t> nprog;
    std::vector<uint16_t> disk;
    std::vector<uint16_t> sysrom;

    std::map<std::string, std::vector<uint16_t>*> mem_regions;
    size_t total_words = 0;

    // ---- Runtime word access (respects WRITEABLE / READABLE) ----
    void write16(const std::string& region, size_t address, uint16_t value);
    uint16_t read16(const std::string& region, size_t address);
    void write16Array(const std::string& region, size_t address,
                      const uint16_t* values, size_t count);
    // Fixed-16 overload kept for existing call sites
    void write16Array(const std::string& region, size_t address, const uint16_t* values);
    uint16_t* read16Array(const std::string& region, size_t address, size_t length);
    void writeString(const std::string& region, size_t address, const std::string& str);
    std::string readString(const std::string& region, size_t address, size_t length);

    // ---- Boot-time loaders (bypass WRITEABLE check) ----
    // Models firmware/cartridge data burned in before the machine boots.
    // Data is little-endian uint16 words.
    bool loadSysrom(const uint16_t* words, size_t count);
    bool loadSysrom(const std::vector<uint16_t>& words);
    bool loadSysromBytes(const uint8_t* bytes, size_t byteCount);  // little-endian stream
    bool loadSysromFile(const std::string& path);

    // Load raw program words straight into NPROG, bypassing WRITEABLE -
    // used by NACPU for both cold boot and disk EXEC (chain-load).
    bool loadNprog(const uint16_t* words, size_t count);

    // ---- Disk filesystem ----
    // `disk` is bulk storage with a tiny flat filesystem baked in (see
    // memory.cpp). loadDiskFile reads a packed .nkg image wholesale;
    // formatDisk() writes an empty-but-valid header when no image is
    // supplied, so the machine still boots to a usable (empty) disk.
    bool loadDiskFile(const std::string& path);
    void formatDisk();

    bool diskFindEntry(const std::string& name, DiskEntry& out);
    bool diskFindBootable(DiskEntry& out);
    bool diskReadEntryWords(const DiskEntry& entry, std::vector<uint16_t>& out);
    // Creates a new entry or overwrites an existing one with the same name.
    // New/grown data is always appended at the free-space pointer - space
    // from a shrunk or deleted entry is not reclaimed in this version.
    bool diskWriteEntry(const std::string& name, uint16_t type, uint16_t flags,
                        const uint16_t* words, size_t count);
    bool diskDeleteEntry(const std::string& name);

private:
    bool mCheck(const std::string& region, size_t address);
    bool rCheck(const std::string& region);
    bool wCheck(const std::string& region);

    // Internal: write into a region without the WRITEABLE gate.
    bool loadReadonly(const std::string& region,
                      const uint16_t* words, size_t count);
    bool loadReadonlyFile(const std::string& region, const std::string& path);
};

#endif //NUEVOAUTO_MEMORY_H
