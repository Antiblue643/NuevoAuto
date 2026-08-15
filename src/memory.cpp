//
// Created by rwarr on 8/6/2026.
//

/*
Memory types (16-bit word architecture)

All regions store 16-bit words. Addresses are word indices.
Capacities below are given in words (same numeric sizes as the previous
byte-oriented design so existing address constants remain valid).

vram     64 Ki words  (~128 KiB)  - display memory
volatile 64 Ki words  (~128 KiB)  - general-purpose RAM + hardware regs
nprog   256 Ki words  (~512 KiB)  - program code (compiled NuPy -> nu)
disk   1024 Ki words (~2096 KiB)  - save data
sysrom   64 Ki words  (~128 KiB)  - read-only firmware / character ROM
*/

#include "memory.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <map>
#include <cstdint>
#include <ranges>
#include <cstring>

constexpr size_t KI = 1024;

Memory::Memory() {
    vram.resize(64 * KI);
    volatile_mem.resize(64 * KI);
    nprog.resize(256 * KI);
    disk.resize(1024 * KI);
    sysrom.resize(64 * KI);

    mem_regions["vram"] = &vram;
    mem_regions["volatile"] = &volatile_mem;
    mem_regions["nprog"] = &nprog;
    mem_regions["disk"] = &disk;
    mem_regions["sysrom"] = &sysrom;

    for (const auto& vec : mem_regions | std::views::values) {
        total_words += vec->size();
    }

    std::cout << "Memory initialized with a size of " << total_words
              << " words (" << (total_words * 2) << " bytes)" << std::endl;
}

// ---------------------------------------------------------------------------
// Bounds / permission checks
// ---------------------------------------------------------------------------
bool Memory::mCheck(const std::string& region, size_t address) {
    if (mem_regions.contains(region)) {
        if (address < mem_regions[region]->size()) {
            return true;
        }
        std::cerr << "Memory Check Error: Address " << address
                  << " out of bounds for region " << region << ".\n";
        return false;
    }
    std::cerr << "Memory Check Error: Unknown memory region " << region << ".\n";
    return false;
}

bool Memory::rCheck(const std::string& region) {
    return std::ranges::find(READABLE, region) != READABLE.end();
}

bool Memory::wCheck(const std::string& region) {
    return std::ranges::find(WRITEABLE, region) != WRITEABLE.end();
}

// ---------------------------------------------------------------------------
// Runtime word access
// ---------------------------------------------------------------------------
void Memory::write16(const std::string& region, size_t address, uint16_t value) {
    if (mCheck(region, address) && wCheck(region)) {
        (*mem_regions[region])[address] = value;
    }
}

uint16_t Memory::read16(const std::string& region, const size_t address) {
    if (mCheck(region, address) && rCheck(region)) {
        return (*mem_regions[region])[address];
    }
    return 0;
}

void Memory::write16Array(const std::string& region, size_t address,
                          const uint16_t* values, size_t count) {
    if (!values || count == 0) return;
    if (!mCheck(region, address) || !wCheck(region)) return;
    if (!mCheck(region, address + count - 1)) return;
    auto& mem = *mem_regions[region];
    for (size_t i = 0; i < count; ++i) {
        mem[address + i] = values[i];
    }
}

void Memory::write16Array(const std::string& region, size_t address,
                          const uint16_t* values) {
    // Legacy fixed-16 overload
    write16Array(region, address, values, 16);
}

uint16_t* Memory::read16Array(const std::string& region, size_t address,
                              size_t length) {
    if (length == 0) return nullptr;
    if (!mCheck(region, address) || !mCheck(region, address + length - 1) ||
        !rCheck(region)) {
        return nullptr;
    }
    const auto& mem = *mem_regions[region];
    auto* result = new uint16_t[length];
    for (size_t i = 0; i < length; ++i) {
        result[i] = mem[address + i];
    }
    return result;
}

void Memory::writeString(const std::string& region, size_t address,
                         const std::string& str) {
    if (!mCheck(region, address) || !wCheck(region)) return;
    if (str.empty()) return;
    if (!mCheck(region, address + str.size() - 1)) return;
    auto& mem = *mem_regions[region];
    for (size_t i = 0; i < str.size(); ++i) {
        mem[address + i] = static_cast<uint16_t>(static_cast<unsigned char>(str[i]));
    }
}

std::string Memory::readString(const std::string& region, size_t address,
                               size_t length) {
    if (length == 0) return "";
    if (!mCheck(region, address) || !mCheck(region, address + length - 1) ||
        !rCheck(region)) {
        return "";
    }
    const auto& mem = *mem_regions[region];
    std::string result;
    result.reserve(length);
    for (size_t i = 0; i < length; ++i) {
        result += static_cast<char>(mem[address + i] & 0xFF);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Boot-time loaders (bypass WRITEABLE)
// ---------------------------------------------------------------------------
bool Memory::loadReadonly(const std::string& region,
                          const uint16_t* words, size_t count) {
    if (!words && count > 0) {
        std::cerr << "Memory: loadReadonly null data for " << region << "\n";
        return false;
    }
    if (!mem_regions.contains(region)) {
        std::cerr << "Memory: unknown region '" << region << "'\n";
        return false;
    }
    auto& dest = *mem_regions[region];
    if (count > dest.size()) {
        std::cerr << "Memory: " << region << " data is " << count
                  << " words, exceeds capacity of " << dest.size() << " words\n";
        return false;
    }
    if (count > 0) {
        std::memcpy(dest.data(), words, count * sizeof(uint16_t));
    }
    // Zero the remainder so stale data never leaks
    if (count < dest.size()) {
        std::memset(dest.data() + count, 0,
                    (dest.size() - count) * sizeof(uint16_t));
    }
    std::cout << "Memory: loaded " << count << " words into " << region << "\n";
    return true;
}

bool Memory::loadReadonlyFile(const std::string& region, const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "Memory: cannot open file for " << region << ": " << path << "\n";
        return false;
    }
    in.seekg(0, std::ios::end);
    const auto byteSize = static_cast<size_t>(in.tellg());
    in.seekg(0, std::ios::beg);

    if (byteSize == 0) {
        std::cerr << "Memory: empty file " << path << "\n";
        return false;
    }
    if (byteSize % 2 != 0) {
        std::cerr << "Memory: " << region
                  << " file length must be even (little-endian uint16 stream): "
                  << path << "\n";
        return false;
    }

    std::vector<uint8_t> bytes(byteSize);
    if (!in.read(reinterpret_cast<char*>(bytes.data()),
                 static_cast<std::streamsize>(byteSize))) {
        std::cerr << "Memory: failed reading " << path << "\n";
        return false;
    }

    // Interpret as little-endian uint16 words
    const size_t wordCount = byteSize / 2;
    std::vector<uint16_t> words(wordCount);
    for (size_t i = 0; i < wordCount; ++i) {
        words[i] = static_cast<uint16_t>(bytes[i * 2]) |
                   (static_cast<uint16_t>(bytes[i * 2 + 1]) << 8);
    }
    return loadReadonly(region, words.data(), words.size());
}

bool Memory::loadSysrom(const uint16_t* words, size_t count) {
    return loadReadonly("sysrom", words, count);
}

bool Memory::loadSysrom(const std::vector<uint16_t>& words) {
    return loadSysrom(words.data(), words.size());
}

bool Memory::loadSysromBytes(const uint8_t* bytes, size_t byteCount) {
    if (!bytes && byteCount > 0) return false;
    if (byteCount % 2 != 0) {
        std::cerr << "Memory: SYSROM byte data length must be even\n";
        return false;
    }
    const size_t wordCount = byteCount / 2;
    std::vector<uint16_t> words(wordCount);
    for (size_t i = 0; i < wordCount; ++i) {
        words[i] = static_cast<uint16_t>(bytes[i * 2]) |
                   (static_cast<uint16_t>(bytes[i * 2 + 1]) << 8);
    }
    return loadSysrom(words.data(), words.size());
}

bool Memory::loadSysromFile(const std::string& path) {
    return loadReadonlyFile("sysrom", path);
}

bool Memory::loadNprog(const uint16_t* words, size_t count) {
    return loadReadonly("nprog", words, count);
}

// ---------------------------------------------------------------------------
// Disk filesystem
//
// The `disk` region (see DISK_* constants in memory.h) is laid out as:
//
//   word 0         magic (DISK_MAGIC)
//   word 1         version (DISK_VERSION)
//   word 2         file count
//   word 3, word 4 next free data offset, hi/lo words (absolute word index
//                  into `disk` - needs two words since the disk is bigger
//                  than 65536 words)
//   words 5..2052  directory table: DISK_MAX_FILES entries x DISK_ENTRY_WORDS
//     each entry: name[8] type flags offsetHi offsetLo lengthHi lengthLo x2 reserved
//     an entry is "free" when every name word is 0.
//   words 2053..   file data, packed back to back as entries are written
//
// offset/length are stored as two words each so a file can be larger than
// 65536 words - the CPU's own SMV/LSM opcodes can't reach past word 65535
// of DISK directly, but the disk-controller commands in NACPU run in host
// code and index this vector directly, so they aren't limited by that.
// ---------------------------------------------------------------------------

namespace {
    size_t diskEntryAddr(size_t index) {
        return DISK_DIR_START + index * DISK_ENTRY_WORDS;
    }

    // Reads a name out of 8 packed words, stopping at the first zero word.
    std::string readPackedName(const uint16_t* words) {
        std::string name;
        for (size_t i = 0; i < DISK_NAME_WORDS; ++i) {
            if (words[i] == 0) break;
            name += static_cast<char>(words[i] & 0xFF);
        }
        return name;
    }

    bool entrySlotFree(const uint16_t* words) {
        for (size_t i = 0; i < DISK_NAME_WORDS; ++i) {
            if (words[i] != 0) return false;
        }
        return true;
    }
}

void Memory::formatDisk() {
    std::fill(disk.begin(), disk.end(), static_cast<uint16_t>(0));
    disk[0] = DISK_MAGIC;
    disk[1] = DISK_VERSION;
    disk[2] = 0; // file count
    disk[3] = static_cast<uint16_t>((DISK_DATA_START >> 16) & 0xFFFF);
    disk[4] = static_cast<uint16_t>(DISK_DATA_START & 0xFFFF);
    std::cout << "Memory: formatted empty disk (" << disk.size() << " words)\n";
}

bool Memory::loadDiskFile(const std::string& path) {
    if (!loadReadonlyFile("disk", path)) {
        return false;
    }
    if (disk.size() < DISK_HEADER_WORDS || disk[0] != DISK_MAGIC) {
        std::cerr << "Memory: " << path << " is not a valid disk image "
                  << "(bad magic) - formatting a blank disk instead\n";
        formatDisk();
        return false;
    }
    return true;
}

bool Memory::diskFindEntry(const std::string& name, DiskEntry& out) {
    for (size_t i = 0; i < DISK_MAX_FILES; ++i) {
        const uint16_t* e = &disk[diskEntryAddr(i)];
        if (entrySlotFree(e)) continue;
        if (readPackedName(e) != name) continue;

        out.index  = static_cast<int>(i);
        out.name   = name;
        out.type   = e[8];
        out.flags  = e[9];
        out.offset = (static_cast<uint32_t>(e[10]) << 16) | e[11];
        out.length = (static_cast<uint32_t>(e[12]) << 16) | e[13];
        return true;
    }
    return false;
}

bool Memory::diskFindBootable(DiskEntry& out) {
    for (size_t i = 0; i < DISK_MAX_FILES; ++i) {
        const uint16_t* e = &disk[diskEntryAddr(i)];
        if (entrySlotFree(e)) continue;
        if (!(e[9] & DISK_FLAG_BOOTABLE)) continue;

        out.index  = static_cast<int>(i);
        out.name   = readPackedName(e);
        out.type   = e[8];
        out.flags  = e[9];
        out.offset = (static_cast<uint32_t>(e[10]) << 16) | e[11];
        out.length = (static_cast<uint32_t>(e[12]) << 16) | e[13];
        return true;
    }
    return false;
}

bool Memory::diskReadEntryWords(const DiskEntry& entry, std::vector<uint16_t>& out) {
    if (entry.index < 0) return false;
    if (static_cast<size_t>(entry.offset) + entry.length > disk.size()) {
        std::cerr << "Memory: disk entry '" << entry.name
                  << "' data range is out of bounds\n";
        return false;
    }
    out.assign(disk.begin() + static_cast<long>(entry.offset),
               disk.begin() + static_cast<long>(entry.offset + entry.length));
    return true;
}

bool Memory::diskWriteEntry(const std::string& name, uint16_t type, uint16_t flags,
                            const uint16_t* words, size_t count) {
    if (name.empty() || name.size() > DISK_NAME_WORDS) {
        std::cerr << "Memory: disk file name must be 1-" << DISK_NAME_WORDS
                  << " characters: '" << name << "'\n";
        return false;
    }
    if (disk.size() < DISK_HEADER_WORDS || disk[0] != DISK_MAGIC) {
        formatDisk();
    }

    // Reuse an existing slot with this name if present, else the first
    // free slot. Either way the payload is always appended fresh at the
    // free-space pointer (see the doc comment above the layout).
    int slot = -1;
    for (size_t i = 0; i < DISK_MAX_FILES; ++i) {
        const uint16_t* e = &disk[diskEntryAddr(i)];
        if (entrySlotFree(e)) {
            if (slot < 0) slot = static_cast<int>(i);
            continue;
        }
        if (readPackedName(e) == name) { slot = static_cast<int>(i); break; }
    }
    if (slot < 0) {
        std::cerr << "Memory: disk directory is full (" << DISK_MAX_FILES << " files)\n";
        return false;
    }

    const uint32_t freeOffset = (static_cast<uint32_t>(disk[3]) << 16) | disk[4];
    if (static_cast<size_t>(freeOffset) + count > disk.size()) {
        std::cerr << "Memory: disk is full, cannot write '" << name << "'\n";
        return false;
    }

    const bool wasFree = entrySlotFree(&disk[diskEntryAddr(static_cast<size_t>(slot))]);

    if (count > 0) {
        std::memcpy(&disk[freeOffset], words, count * sizeof(uint16_t));
    }

    uint16_t* e = &disk[diskEntryAddr(static_cast<size_t>(slot))];
    std::memset(e, 0, DISK_ENTRY_WORDS * sizeof(uint16_t));
    for (size_t i = 0; i < name.size(); ++i) {
        e[i] = static_cast<uint16_t>(static_cast<unsigned char>(name[i]));
    }
    e[8]  = type;
    e[9]  = flags;
    e[10] = static_cast<uint16_t>((freeOffset >> 16) & 0xFFFF);
    e[11] = static_cast<uint16_t>(freeOffset & 0xFFFF);
    const auto newLen = static_cast<uint32_t>(count);
    e[12] = static_cast<uint16_t>((newLen >> 16) & 0xFFFF);
    e[13] = static_cast<uint16_t>(newLen & 0xFFFF);

    const uint32_t newFree = freeOffset + static_cast<uint32_t>(count);
    disk[3] = static_cast<uint16_t>((newFree >> 16) & 0xFFFF);
    disk[4] = static_cast<uint16_t>(newFree & 0xFFFF);

    if (wasFree) {
        disk[2] = static_cast<uint16_t>(disk[2] + 1);
    }
    return true;
}

bool Memory::diskDeleteEntry(const std::string& name) {
    for (size_t i = 0; i < DISK_MAX_FILES; ++i) {
        uint16_t* e = &disk[diskEntryAddr(i)];
        if (entrySlotFree(e)) continue;
        if (readPackedName(e) != name) continue;
        std::memset(e, 0, DISK_ENTRY_WORDS * sizeof(uint16_t));
        disk[2] = static_cast<uint16_t>(disk[2] > 0 ? disk[2] - 1 : 0);
        return true;
    }
    return false;
}
