#include "memory.h"
#include "november.h"
#include "NACPU.h"
#include "afterburnerII.h"
#include "sysres_gen.h"
#include "input.h"
#include "debugger.h"

#include <SDL3/SDL.h>

#include <iostream>
#include <string>
#include <cstring>
#include <random>

static void printUsage(const char* argv0) {
    std::cerr
        << "Usage: " << argv0 << " [options] [program.nu]\n"
        << "  --disk <file>       Boot from a packed .nkg disk image (mkdisk.py output).\n"
        << "                      Overrides program.nu - the disk's bootable\n"
        << "                      entry becomes the running program.\n"
        << "                      (default: disk.nkg)\n"
        << "  --sysrom <file>     Load raw little-endian uint16 SYSROM binary\n"
        << "                      (skips the built-in font/keymap)\n"
        << "  --chars-png <file>  Use a PNG font atlas instead of the built-in font\n"
        << "                      (pairs with --chars-txt; default: font_std.png)\n"
        << "  --chars-txt <file>  Use a text glyph map instead of the built-in charmap\n"
        << "                      (pairs with --chars-png; default: charmap.txt)\n"
        << "  --no-sysres         Skip SYSROM load entirely\n"
        << "  -h, --help          Show this help\n"
        << "\n"
        << "Default: burn the built-in font+charmap (font_std.h/charmap.h) into\n"
        << "SYSROM, boot from disk.nkg. --chars-png/--chars-txt opt back into\n"
        << "loading those from disk instead, e.g. for iterating on a new font.\n";
}

int main(int argc, char* argv[]) {
    std::string nuPath = "disk/mk.nub";
    std::string sysromPath;
    std::string romPath;
    std::string diskPath = "disk.nkg";
    std::string charsPng = DEFAULT_CHARS_PNG;
    std::string charsTxt = DEFAULT_CHARS_TXT;
    bool noSysres = false;
    bool useFileCharset = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--disk") == 0 && i + 1 < argc) {
            diskPath = argv[++i];
        } else if (std::strcmp(argv[i], "--sysrom") == 0 && i + 1 < argc) {
            sysromPath = argv[++i];
        } else if (std::strcmp(argv[i], "--chars-png") == 0 && i + 1 < argc) {
            charsPng = argv[++i];
            useFileCharset = true;
        } else if (std::strcmp(argv[i], "--chars-txt") == 0 && i + 1 < argc) {
            charsTxt = argv[++i];
            useFileCharset = true;
        } else if (std::strcmp(argv[i], "--no-sysres") == 0) {
            noSysres = true;
        } else if (std::strcmp(argv[i], "-h") == 0 ||
                   std::strcmp(argv[i], "--help") == 0) {
            printUsage(argv[0]);
            return 0;
        } else if (argv[i][0] == '-') {
            std::cerr << "Unknown option: " << argv[i] << "\n";
            printUsage(argv[0]);
            return 1;
        } else {
            nuPath = argv[i];
        }
    }

    Memory mem;

    // Boot-time SYSROM: raw binary override > explicit PNG/txt override >
    // built-in font_std.h/charmap.h (default, no files needed at all).
    if (!noSysres) {
        if (!sysromPath.empty()) {
            if (!mem.loadSysromFile(sysromPath)) {
                std::cerr << "Failed to load SYSROM from " << sysromPath << "\n";
                return 1;
            }
        } else if (useFileCharset) {
            if (!loadSysres(mem, charsPng, charsTxt)) {
                std::cerr << "Warning: loadSysres failed "
                          << "(missing " << charsPng << " / " << charsTxt
                          << "?). Continuing with empty SYSROM.\n";
            }
        } else {
            if (!loadSysresBuiltin(mem)) {
                std::cerr << "Warning: loadSysresBuiltin failed. "
                          << "Continuing with empty SYSROM.\n";
            }
        }
    }

    if (!diskPath.empty()) {
        // loadDiskFile formats a blank disk itself on failure/bad magic,
        // so the machine still comes up; bootFromDisk() below is what
        // actually fails the run if there's nothing bootable on it.
        mem.loadDiskFile(diskPath);
    }

    November nov(mem);
    if (nov.init() != 0) {
        std::cerr << "Failed to initialise November display\n";
        return 1;
    }

    NACPU cpu(mem, nov);
    Input input(mem, nov);
    Debugger debugger(mem, cpu, nov);
    input.setDebugger(&debugger);

    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> entropyDist(0x0000, 0xFFFF);

    AfterburnerII audio(mem, cpu, 44100);
    if (audio.init() != 0) {
        std::cerr << "Warning: audio device init failed, running without sound.\n";
    }

    if (!diskPath.empty()) {
        if (!cpu.bootFromDisk()) {
            std::cerr << "Failed to boot from disk: " << diskPath << "\n";
            return 1;
        }
    } else if (!cpu.loadNuFile(nuPath)) {
        std::cerr << "Failed to load program: " << nuPath << "\n";
        return 1;
    }

    constexpr int MAX_INSTRS_PER_FRAME = 200000;
    constexpr Uint64 TARGET_FRAME_NS  = 1'000'000'000ull / 60; // ~16.67 ms

    Uint64 nextFrame = SDL_GetTicksNS();

    while (cpu.isRunning() && !nov.shouldQuit()) {
        // Host work once per frame
        mem.write16("volatile", 0x80FE, static_cast<uint16_t>(entropyDist(rng)));
        mem.write16("volatile", 0x80FF, static_cast<uint16_t>(entropyDist(rng)));
        input.captureInput();
        cpu.updateHardwareFlags();

        for (int n = 0; n < MAX_INSTRS_PER_FRAME; ++n) {
            if (!cpu.isRunning()) break;
            Instruction ins = cpu.fetch();
            if (ins.opcode_name.empty()) break;
            cpu.operate(ins);
            cpu.checkInterrupts();
            if (ins.opcode_name == "FLP") break;
        }


        audio.update();

        nextFrame += TARGET_FRAME_NS;
        const Uint64 now = SDL_GetTicksNS();
        if (now < nextFrame) {
            SDL_DelayNS(nextFrame - now);
        } else {
            // Lagged: resync so we don't try to catch up with a burst
            // of near-zero-duration frames.
            nextFrame = now;
        }
    }

    return 0;
}