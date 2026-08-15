# NuPy

**NuPy** is a restricted Python subset that compiles to NACPU machine code for the NuevoAuto virtual machine. Source files use the `.nupy` extension. The compiler (`compile.py`) turns them into either human-readable `.nu` assembly text or a raw little-endian uint16 binary (`.nub`).

It looks and feels like Python, but it is *not* CPython: there is no heap, no objects, no runtime string/list types, and a hard register budget. Programs talk to the machine through memory regions and a fixed set of intrinsics. Does make things easier though.

---

## Quick start

```bash
# Text assembly (.nu)
python tools/compile.py myprog.nupy

# Binary (.nub) - preferred for packing into disk images
python tools/compile.py --bin myprog.nupy
```

Typical source starts with:

```python
from nupy import *

# ... functions and main code ...
```

`nupy.py` is only a stub module for editors/linters. The compiler ignores the import and provides the real intrinsics itself.

Entry point is module-level code (after all `def`s). The compiler emits a jump over function bodies, then a `main` label, then your top-level statements, ending with `BRK`.

---

## Language features

### Types

Everything is a **16-bit integer** (word). Bools are `0` / `1`. There are no floats, strings, lists, dicts, or classes at runtime.

| Concept   | How it works                                                               |
|-----------|----------------------------------------------------------------------------|
| Integers  | `0` ... `65535` (signed range -32768...32767 also accepted for immediates) |
| Bools     | `True` / `False` -> `1` / `0`                                              |
| "Strings" | Buffers of character codes in a memory region + a length                   |
| "Arrays"  | Compile-time constant lists only, or explicit memory buffers               |

### Variables & registers

- Local variables map to **R0-R10** (11 slots per function / main scope).
- Temporaries for expression evaluation: **R11-R14** (max nesting depth 4).
- Return value: **R15**.
- Exceeding 11 locals in one function is a compile error - spill to `VOLATILE` memory yourself if you need more.

Parameters are assigned the first N variable registers (max **4** parameters per function).

### Functions

```python
def add(a, b):
    return a + b

x = add(3, 4)
```

- No recursion (direct or indirect) - the compiler rejects call cycles.
- Arguments and live registers are spilled to VOLATILE around `CAL` / `RET`.
- `return` with a value puts it in R15; bare `return` is fine.

### Control flow

Supported:

- `if` / `elif` / `else`
- `while`
- `for x in range(n)` and `for x in range(start, stop)`
    - Bound must be a **constant**; step must be 1 (no `range(a, b, step)`)
- `break` / `continue`
- `pass`
- Boolean conditions: `and`, `or`, `not`, comparisons (`== != < <= > >=`)

Not supported: `match`, generators, comprehensions, `try`/`except`, `with`, `async`, chained comparisons (`a < b < c`).

Special condition forms:

- `while displayLoop():` - loops while the host "keep running" flag is set
- `while halt():` - loops while the halt flag is clear
- `if readString(region, addr, length) == "literal":` - compile-time string compare (only as a direct condition)

### Operators

| Category   | Operators                                        |
|------------|--------------------------------------------------|
| Arithmetic | `+` `-` `*` `//` (floor div)                     |
| Bitwise    | `&` `\|` `^` `~`                                 |
| Shifts     | `<<` `>>` (shift amount **must be a constant**)  |
| Unary      | `-` (NEG), `~` (NOT)                             |
| Augmented  | `+=` `-=` `*=` `//=` `&=` `\|=` `^=` `<<=` `>>=` |

No `%`, `**`, true division `/`, or floating-point ops.

### Memory regions

Five regions, accessed by name (string literals or the subscript sugar):

| Region     | Write?        | Typical use                 |
|------------|---------------|-----------------------------|
| `VOLATILE` | yes           | RAM, stack, MMIO, buffers   |
| `DISK`     | yes           | Persistent filesystem image |
| `VRAM`     | yes           | November display memory     |
| `NPROG`    | yes           | Program code space          |
| `SYSROM`   | **read-only** | Font / firmware ROM         |

Two equivalent styles:

```python
# Function style
write("VOLATILE", 0x1000, 42)
v = read("VOLATILE", 0x1000)

# Subscript style (region names end with _)
VOLATILE_[0x1000] = 42
v = VOLATILE_[0x1000]
```

The `*_` names (`VOLATILE_`, `DISK_`, `VRAM_`, `NPROG_`, `SYSROM_`) are the preferred identifiers for subscript access (see `nupy.py`).

### Constant arrays

```python
palette = [0, 15, 240, 4095]   # compile-time only
writeArray("VRAM", MASTER_PALETTE_BASE, palette)
# palette[2] is OK if the index is a constant
# palette[i] with a variable index is not
```

Constant arrays cannot be mutated after definition.

---

## Intrinsics

### Core / memory

| Call                                                    | Meaning                                                      |
|---------------------------------------------------------|--------------------------------------------------------------|
| `write(region, addr, val)`                              | Store one word                                               |
| `read(region, addr)`                                    | Load one word                                                |
| `writeArray(region, addr, list_or_const_array)`         | Store a sequence of constant words                           |
| `writeString(region, addr, "text")`                     | Store character codes (literal only)                         |
| `copyString(dst_reg, dst, src_reg, src, length)`        | Copy `length` words between regions                          |
| `splitString(region, addr, length, out_base, max_args)` | Tokenize on spaces into a VOLATILE table (`max_args` 1...16) |
| `strFind(region, addr, length, char)`                   | Index of first matching character, or `length` if not found  |
| `readString(...)`                                       | **Only** in `==` / `!=` conditions against a string literal  |
| `readArray(...)`                                        | Statement only (debug print of a buffer); no return value    |

### Display / November

| Call                                                    | Meaning                                                |
|---------------------------------------------------------|--------------------------------------------------------|
| `cls()`                                                 | Clear the screen                                       |
| `frameBlit()`                                           | Flip / present the frame (`FLP`)                       |
| `wait(frames)`                                          | Wait until hardware frame counter advances by `frames` |
| `setPixel(x, y, color)`                                 | Set a pixel                                            |
| `drawLine(x0, y0, x1, y1, color)`                       | Draw a line                                            |
| `getPixel(x, y)`                                        | Read a pixel color                                     |
| `setMasterColor(index, r, g, b)`                        | 12-bit master palette entry (0-4095)                   |
| `setActivePalette(slot, master_index)`                  | Map on-screen color 0-63 -> master                     |
| `writeBitplaneWord(plane, word_idx, val)`               | Write 16 packed pixels to a bitplane                   |
| `setSpritePos(id, x, y)`                                | Sprite position                                        |
| `setSpriteAttr(id, flags, scale_x, scale_y, rot, skew)` | Enable / flip / scale / rotate                         |
| `setSpriteColor(id, pal_idx, master_idx)`               | Sprite local palette entry                             |
| `setSplitScreen(enabled, axis, pos)`                    | Hardware split                                         |
| `setHScroll(zone_a, zone_b)`                            | Per-zone horizontal scroll                             |

`FRC_` is the read-only hardware frame counter (maps to a fixed VOLATILE address).

### Audio / Afterburner II

| Call                                         | Meaning                                                 |
|----------------------------------------------|---------------------------------------------------------|
| `setPSGVolume(master, psg1, psg2)`           | Volumes 0-15                                            |
| `setPSG3Control(volume, wave_enable)`        | PSG3 vol + wavetable enable                             |
| `setPSGWaveform(w0, w1, w2, bitwise_mode=0)` | 2-bit wave per channel + optional combine mode          |
| `setPSGDuty(d0, d1, d2)`                     | 4-bit duty / saw flip                                   |
| `setPSGTone(channel, period)`                | Tone period (channel 0-2)                               |
| `setPSG3Wavetable(index, word_val)`          | Wavetable RAM word 0-7                                  |
| `note_to_period("A4")`                       | **Compile-time** fold to period immediate               |
| `freq_to_period(440)`                        | **Compile-time** fold to period immediate               |
| `note_to_freq("A4")`                         | Folds to rounded Hz (prefer `note_to_period` for tones) |

Chip clock is 4 MHz; period formula is `round(4e6 / (64 * freq))`, clamped to 1...65535 (0 = silence).

### Interrupts & stack

| Call                                         | Meaning                |
|----------------------------------------------|------------------------|
| `enableInterrupts()` / `disableInterrupts()` | SEI / CLI              |
| `returnFromInterrupt()`                      | RTI                    |
| `softwareInterrupt()`                        | SWI                    |
| `push(x)` / `pop(x)`                         | Stack ops via SP       |
| `setStackPointer(x)`                         | Set SP (e.g. `0x7F00`) |
| `setIrqVector(source, addr)`                 | IRQ vector             |
| `setIrqMask(mask)`                           | IRQ mask               |
| `setTimer(reload)`                           | Hardware timer         |
| `clearIrq(source)`                           | Clear pending bit      |

### Disk

| Call                                 | Meaning                                                                                |
|--------------------------------------|----------------------------------------------------------------------------------------|
| execProgram(name)                    | One-way call to another program, overwriting NPROG. Basically "swapping carts".        |
| diskSave(name, region, addr, length) | Save `length` from `region`, `addr` under `name` to disk.                              |
| diskDelete(name)                     | Delete the file with the given name from disk.                                         |
| diskLoad(name, region, addr)         | Load data from the file with the given name into the specified `region` and `address`. |


### System

| Call                       | Meaning                                      |
|----------------------------|----------------------------------------------|
| `shutdown()`               | Request host exit                            |
| `displayLoop()` / `halt()` | Special condition helpers (see control flow) |

Useful constants are defined in `nupy.py` (VRAM layout, audio ports, entropy addresses `ENT1_` / `ENT2_`, etc.).

---

## Limitations (summary)

| Limit                   | Detail                                                                                                                                    |
|-------------------------|-------------------------------------------------------------------------------------------------------------------------------------------|
| Locals per function     | <= 11 (R0-R10)                                                                                                                            |
| Parameters per function | <= 4                                                                                                                                      |
| Expression depth        | <= 4 temporary registers                                                                                                                  |
| Recursion               | Forbidden                                                                                                                                 |
| `for` loops             | `range` only; stop bound constant; step = 1                                                                                               |
| Shift amounts           | Must be compile-time constants                                                                                                            |
| Strings / lists         | No runtime types - only buffers + lengths, or constant arrays                                                                             |
| Types                   | Integers (and bools as 0/1) only                                                                                                          |
| Unsupported Python      | classes, exceptions, imports (ignored), generators, comprehensions, `with`, `async`, `%`, `**`, true `/`, chained compares, `match`, etc. |
| SYSROM                  | Read-only                                                                                                                                 |
| FRC_                    | Read-only                                                                                                                                 |
| len()                   | Compiles to immediate value, only constant arguments                                                                                      |


If you hit the register ceiling, move state into VOLATILE (or DISK) and keep only hot indices in registers - the sample kernel (`../disk/mk.nupy`) does exactly that for the command buffer and cursor.

---

## Compilation model

1. Parse with Python's `ast` module.
2. Lower each function and the module body to NACPU opcodes.
3. Resolve labels; emit either:
    - **Text** `.nu` - one instruction per line, comments preserved
    - **Binary** `.nub` - little-endian uint16 words (opcode + operands)

Opcode table and operand counts are kept in lockstep with `NACPU.h`. Changing either side without the other will break programs.

Pack binaries into a bootable disk image with `mkdisk.py`:

```bash
python tools/compile.py --bin myprog.nupy
python tools/mkdisk.py --boot myprog some_dir/ mydisk.nkg
./NuevoAuto --disk mydisk.nkg
```

---

## Example (minimal)

```python
from nupy import *

def main():
    setStackPointer(0x7F00)
    cls()
    setActivePalette(0, 0)
    setActivePalette(1, 4095)
    setPixel(160, 120, 1)
    frameBlit()
    wait(60)
    shutdown()

main()
```

See `../disk/mk.nupy` for a fuller example: font rendering from SYSROM, a simple command shell, PSG chime, and interrupt setup.

---

## Editor support

Import `from nupy import *` so your IDE sees the stub signatures in `nupy.py`. The stubs are no-ops at runtime; only the compiler implements them.