# NuevoAuto

A custom 16-bit virtual machine / retro computer emulator. It emulates a complete little machine with its own CPU, graphics chip, sound chip, memory map, and disk filesystem, hosted on SDL3.

Programs are written in a restricted Python subset called **NuPy**, compiled to native `.nu` / `.nub` binaries, and run on the **NACPU**.

---

## Specs

### Architecture overview

| Component | Name               | Role                              |
|-----------|--------------------|-----------------------------------|
| CPU       | **NACPU**          | 16-bit custom ISA, word-addressed |
| Display   | **November**       | 320x240, bitplanes + sprites      |
| Audio     | **Afterburner II** | 3-channel PSG + wavetable         |
| Host      | SDL3 + Dear ImGui  | Window, input, debugger UI        |

### Memory map (16-bit words)

All addresses are **word indices**. Capacities:

| Region     | Size (words) | Approx. bytes | Notes                                                      |
|------------|--------------|---------------|------------------------------------------------------------|
| `vram`     | 64 Ki        | ~128 KiB      | Display memory (bitplanes, sprites, palettes, scroll regs) |
| `volatile` | 64 Ki        | ~128 KiB      | General-purpose RAM + MMIO / stack / IRQ regs              |
| `nprog`    | 256 Ki       | ~512 KiB      | Program code space                                         |
| `disk`     | 1024 Ki      | ~2 MiB        | Persistent filesystem image                                |
| `sysrom`   | 64 Ki        | ~128 KiB      | Read-only firmware / character ROM                         |

**Total = ~1472 Ki words (~2.9 MiB).**

Key fixed locations (see headers for the full map):

- Stack: `0x7000`-`0x7F00` (volatile)
- GPU ports: `0xFFF0`-`0xFFF6`
- Disk controller: `0xFFE0`-`0xFFE5`
- IRQ / timer block: `0x00F0`-`0x00F8` ...
- Audio (Afterburner II): `0x9000`-`0x9017`

### NACPU

- 16-bit registers and ALU
- Custom opcode set (`NOP`, `MOV`/`SMV`/`VMV`, arithmetic, shifts, conditional jumps, `CAL`/`RET`, stack ops, interrupt control, etc.)
- Direct and register-indirect memory access across the five regions
- Hardware interrupts: VBlank, Input, Timer, Software
- Frame-driven execution (up to 200 000 instructions per 60 Hz frame)

### November (graphics)

- Native resolution: **320x240**
- **6 bitplanes** (packed 16 px/word)
- **16 sprites**, 32x32, 4 bpp, with per-sprite attributes (position, enable, priority, flip, scale, rotation, skew) and private 16-color palettes
- 4096-entry master palette (12-bit RGB) + 64 active background colors
- Split-screen and independent horizontal scroll for each zone
- `FLP` opcode / GPU commands flip the display buffer

### Afterburner II (audio)

- 3 PSG channels (tone + noise/waveform modes, duty cycle)
- 32-step 4-bit wavetable
- Chip clock: **4 MHz**
- Host sample rate: 44.1 kHz stereo (SDL3 audio stream)

### Disk filesystem (`.nkg`)

Custom packed image format (magic `0x4E4B` / "NK"):

- Header + directory (up to 128 entries)
- Each entry: 8-word name, type, flags (bootable / read-only), offset, length
- Types: program, graphics, font, data, other
- Controller commands: STAT / LOAD / SAVE / DELETE / EXEC (chain-load)

A pre-built image `disk.nkg` and a sample boot program source (`disk/mk.nupy`) are included. `mkdisk.py` compiles `.nupy` sources on the fly when packing a disk, so no separate `.nub` needs to be checked in.

---

## Building

### Requirements

- CMake >= 3.20
- C++20 compiler (GCC, Clang, or MSVC / MinGW)
- Git (CMake FetchContent pulls SDL3 and Dear ImGui)
- Python 3 (for the toolchain under `tools/`)

No system SDL or ImGui install is required; both are fetched and built statically.

### Configure & build

```bash
mkdir build && cd build
cmake ..
cmake --build .
```

The executable is named `NuevoAuto`. Post-build steps copy the `disk/` and `emures/` asset directories, plus the top-level `disk.nkg` image, next to the binary.

**Note**:
On MinGW/Windows the CMake file requests a fully static link.

---

## Running

```text
NuevoAuto [options] [program.nu]

  --disk <file>       Boot from a packed .nkg disk image.
                      Overrides the program argument; the disk's
                      bootable entry is loaded into nprog and run.
                      (default: disk.nkg)
  --sysrom <file>     Load a raw little-endian uint16 SYSROM binary
                      (skips auto-generation from the font assets).
  --chars-png <file>  Font atlas PNG (default: emures/font_std.png)
  --chars-txt <file>  Glyph name map (default: emures/charmap.txt)
  --no-sysres         Skip SYSROM load entirely
  -h, --help          Show help
```

**Defaults:**

- Build SYSROM from `font_std.h` + `charmap.h`
- Boot from `disk.nkg`'s bootable entry
- Window/exe icon from `emures/icon.png` (window) / `emures/icon.ico` (exe resource)

`program.nu` and `--chars-png`/`--chars-txt` are only used if you opt out of a default above (no `--disk`, or `--no-sysres` isn't set, but you still want to load the font from files).

Example:

```bash
./NuevoAuto
# boots disk.nkg by default
./NuevoAuto --disk path/to/other.nkg
# or
./NuevoAuto path/to/myprogram.nu
```

---

## Toolchain (`tools/`)

| Script          | Purpose                                                               |
|-----------------|-----------------------------------------------------------------------|
| `compile.py`    | Compiles NuPy (Python subset) -> `.nu` text assembly or binary `.nub` |
| `mkdisk.py`     | Packs a directory of programs/assets into a `.nkg` disk image         |
| `nupy.py`       | Helper stubs / constants for editors and linters                      |
| `commentgen.py` | Used by the compiler for generated comments                           |

Typical workflow:

```bash
# Optional: compile a NuPy source to a standalone .nub binary
python tools/compile.py --bin myprog.nupy

# Build a bootable disk image - mkdisk.py compiles .nupy sources itself,
# so this step alone is enough even if you skipped the one above
# (marks the sole program in src_dir/ as bootable if --boot is omitted)
python tools/mkdisk.py --boot myprog src_dir/ mydisk.nkg

# Run it
./build/NuevoAuto --disk mydisk.nkg
```

The compiler's opcode table and the disk layout constants are kept in sync with the C++ headers (`NACPU.h`, `memory.h`).

---

## Project layout

```text
NuevoAuto/
├── CMakeLists.txt
├── disk/                 # Default assets
│   ├── mk.nupy           # Sample boot program source
│   └── test.nupy
├── disk.nkg              # Packed disk image - default boot target
├── emures/               # Emulator UI/host resources
│   ├── icon.png          # Window icon (loaded at runtime)
│   ├── icon.ico           # Exe icon (embedded via src/app.rc at build time)
│   ├── colors.ncp         # Boot palette
│   ├── font_std.png / charmap.txt  # Font atlas + glyph map (--chars-png/--chars-txt)
│   └── fontmaker.py, formatcharmap.py
├── src/
│   ├── main.cpp          # Entry point, CLI, main loop
│   ├── NACPU.{h,cpp}     # CPU + ISA
│   ├── memory.{h,cpp}    # Regions, disk FS, SYSROM loader
│   ├── november.{h,cpp}  # Graphics chip (also sets the window icon)
│   ├── afterburnerII.{h,cpp}  # Sound chip
│   ├── input.{h,cpp}
│   ├── debugger.{h,cpp}
│   ├── sysres_gen.{h,cpp}     # Font -> SYSROM builder
│   ├── fileutil.{h,cpp}
│   ├── charmap.h # Builtin character map
│   ├── font_std.h # Builtin font atlas
│   ├── app.rc # Windows exe icon resource script
│   └── extern/stb_image.h
└── tools/                # Python compiler & disk tools
```

---

## Development notes

- The machine targets ~60 FPS; the host injects entropy into volatile memory each frame and advances audio after the instruction budget or an explicit `FLP`.
- Interrupt sources and GPU/disk ports are memory-mapped; see the `#define`s at the top of `NACPU.h` and the VRAM layout in `november.h`.
- SYSROM is normally generated at startup from the character atlas; you can also supply a pre-built binary with `--sysrom`.

Enjoy building on NuevoAuto!