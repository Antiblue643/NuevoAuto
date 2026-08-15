# NuevoAuto Hardware Reference

Programming model for the guest machine: memory regions, memory-mapped I/O, graphics (November), audio (Afterburner II), interrupts, input, disk filesystem, and SYSROM.

All addresses are **16-bit word indices** unless noted. Values are 16-bit words. Region names in NuPy are `VOLATILE`, `DISK`, `VRAM`, `NPROG`, `SYSROM`.

For the language that targets this machine, see [`docs/NuPy.md`](../docs/NuPy.md).

---

## Memory regions

| Region     | Size (words) | Bytes (approx.) | Access | Role                         |
|------------|--------------|-----------------|--------|------------------------------|
| `vram`     | 64 Ki        | ~128 KiB        | R/W    | November display memory      |
| `volatile` | 64 Ki        | ~128 KiB        | R/W    | RAM, stack, MMIO, IRQ, input |
| `nprog`    | 256 Ki       | ~512 KiB        | R/W    | Program code                 |
| `disk`     | 1024 Ki      | ~2 MiB          | R/W    | Packed filesystem image      |
| `sysrom`   | 64 Ki        | ~128 KiB        | **RO** | Font + keymap firmware       |

CPU load/store opcodes are region-specific (`MOV`/`LDM` for volatile, `SMV`/`LSM` for disk, `VMV`/`LVM` for VRAM, `SNP`/`LNP` for nprog, `LBM`/`LBMI` for sysrom). Register-indirect forms end in `I` (`MOVI`, `LDMI`, ...).

---

## VOLATILE map (important fixed addresses)

| Address     | Name / use                                                     |
|-------------|----------------------------------------------------------------|
| `0x00F0`    | `IRQ_PENDING`                                                  |
| `0x00F1`    | `IRQ_MASK`                                                     |
| `0x00F2`    | `IRQ_STATUS` (mirror of pending & mask)                        |
| `0x00F3`    | `IRQ_TIMER_RELOAD`                                             |
| `0x00F4`    | `IRQ_TIMER_COUNT`                                              |
| `0x00F8`... | `IRQ_VECTOR_BASE` - one word per source (8 sources)            |
| `0x00FD`    | Frame counter (`FRC_` in NuPy) - host frame index, low 16 bits |
| `0x00FE`    | Halt requested (nonzero -> machine should stop)                |
| `0x00FF`    | Display active (0 -> host is quitting; used by `displayLoop`)  |
| `0x0100`... | Compiler save-slot area (keep IRQ block free)                  |
| `0x7000`    | `STACK_LIMIT`                                                  |
| `0x7F00`    | `STACK_TOP` (typical SP after boot)                            |
| `0x8000`... | Input ports (see [Input](#input))                              |
| `0x80FE`    | Entropy word 1 (host random each frame)                        |
| `0x80FF`    | Entropy word 2                                                 |
| `0x9000`... | Afterburner II audio (see [Audio](#afterburner-ii-audio))      |
| `0xFFE0`... | Disk controller (see [Disk controller](#disk-controller-mmio)) |
| `0xFFF0`... | GPU command ports (see [GPU ports](#gpu-command-ports))        |

Guest code should treat `0x00F0`-`0x00FF` and the I/O blocks as reserved. The NuPy compiler's spill base starts at `0x0100` for this reason.

---

## GPU command ports

Located in **VOLATILE**. Write the argument registers, then write `GPU_CMD` to trigger.

| Address  | Name         | Role                    |
|----------|--------------|-------------------------|
| `0xFFF0` | `GPU_CMD`    | Command (write to fire) |
| `0xFFF1` | `GPU_X0`     | X0                      |
| `0xFFF2` | `GPU_Y0`     | Y0                      |
| `0xFFF3` | `GPU_X1`     | X1 (line)               |
| `0xFFF4` | `GPU_Y1`     | Y1 (line)               |
| `0xFFF5` | `GPU_COLOR`  | Color index             |
| `0xFFF6` | `GPU_RESULT` | Result of GETPIXEL      |

| `GPU_CMD` value | Operation                                          |
|-----------------|----------------------------------------------------|
| `1`             | `SETPIXEL` - set pixel (X0, Y0) to COLOR           |
| `2`             | `GETPIXEL` - read pixel (X0, Y0) -> RESULT         |
| `3`             | `DRAWLINE` - line from (X0,Y0) to (X1,Y1) in COLOR |

NuPy maps these to `setPixel`, `getPixel`, `drawLine`. Presenting the frame is the `FLP` opcode / `frameBlit()` intrinsic (also raises VBlank IRQ).

Writing `0xFFFF` as a VRAM store address is treated as a clear in some paths; prefer `cls()` / November clear helpers.

---

## November (graphics)

Native resolution: **320x240**. All layout addresses are **word offsets into the `vram` region**.

### Bitplanes

| Constant              | Value | Notes                     |
|-----------------------|-------|---------------------------|
| `NATIVE_WIDTH`        | 320   |                           |
| `NATIVE_HEIGHT`       | 240   |                           |
| `NUM_PLANES`          | 6     |                           |
| `PLANE_WORDS_PER_ROW` | 20    | 16 pixels packed per word |
| `PLANE_SIZE`          | 4800  | words per plane           |
| `BITPLANE_BASE`       | 0     |                           |
| `BITPLANE_TOTAL`      | 28800 | 6 x 4800                  |

Plane *p*, row *y*, word *w*:  
`BITPLANE_BASE + p * PLANE_SIZE + y * PLANE_WORDS_PER_ROW + w`

Each word holds 16 horizontal pixels (MSB = leftmost). Background color for a pixel is formed from the six plane bits as a 0-63 index into the **active palette**.

### Sprites

| Constant              | Value | Notes                             |
|-----------------------|-------|-----------------------------------|
| `SPRITE_COUNT`        | 16    |                                   |
| `SPRITE_SIZE`         | 32    | 32x32 pixels                      |
| `SPRITE_PIXELS_BASE`  | 28800 |                                   |
| `SPRITE_PIXEL_WORDS`  | 256   | 4 bpp -> 4 px/word -> 8 words/row |
| `SPRITE_PIXELS_TOTAL` | 4096  |                                   |
| `SPRITE_ATTRS_BASE`   | 32896 |                                   |
| `SPRITE_ATTR_WORDS`   | 8     | per sprite                        |
| `SPRITE_ATTRS_TOTAL`  | 128   |                                   |
| `SPRITE_PAL_BASE`     | 33024 |                                   |
| `SPRITE_PAL_TOTAL`    | 256   | 16 colors x 16 sprites            |

**Attribute record** (8 words per sprite, starting at `SPRITE_ATTRS_BASE + id * 8`):

| Word | Field    | Meaning                                                          |
|------|----------|------------------------------------------------------------------|
| 0    | X        | Screen X (signed; off-screen allowed)                            |
| 1    | Y        | Screen Y (signed)                                                |
| 2    | flags    | bit0 enable, bit1 priority (behind BG), bit2 flip_h, bit3 flip_v |
| 3    | scale_x  | Q8.8 fixed point (`256` = 1.0x)                                  |
| 4    | scale_y  | Q8.8                                                             |
| 5    | rotation | Degrees 0-359                                                    |
| 6    | skew     | Signed degrees (shear)                                           |
| 7    | reserved |                                                                  |

Sprite pixels are 4 bpp; each sprite has its own 16-entry palette of **master palette indices**.

### Palettes & scroll

| Constant               | Value | Notes                                     |
|------------------------|-------|-------------------------------------------|
| `MASTER_PALETTE_BASE`  | 33280 | 4096 entries                              |
| `MASTER_PALETTE_TOTAL` | 4096  |                                           |
| `ACTIVE_PALETTE_BASE`  | 37376 | 64 on-screen BG colors                    |
| `ACTIVE_PALETTE_TOTAL` | 64    |                                           |
| `SPLIT_ENABLE`         | 37440 | Nonzero = split on                        |
| `SPLIT_AXIS`           | 37441 | 0 = horizontal (top/bottom), 1 = vertical |
| `SPLIT_POS`            | 37442 | Row 0-239 or column 0-319                 |
| `HSCROLL_A`            | 37443 | Zone A scroll (signed, wraps)             |
| `HSCROLL_B`            | 37444 | Zone B scroll                             |
| `REGISTER_MAP_END`     | 37445 |                                           |

**Master color packing** (one word):  
bits `11:8` = R, `7:4` = G, `3:0` = B (nibbles 0-15, scaled x17 -> 0-255 on the host).

Active palette slots 0-63 store indices into the master palette (0-4095).

NuPy helpers: `setMasterColor`, `setActivePalette`, `setSpritePos`, `setSpriteAttr`, `setSpriteColor`, `setSplitScreen`, `setHScroll`, `writeBitplaneWord`, `cls`, `frameBlit`.

---

## Afterburner II (audio)

MMIO in **VOLATILE**. Chip clock: **4 MHz**. Host plays stereo S16 at 44.1 kHz.

| Address           | Name               | Layout / meaning                                                  |
|-------------------|--------------------|-------------------------------------------------------------------|
| `0x9000`          | `AUDIO_PSG_VOL012` | `0x0V12` - master V, PSG1 vol, PSG2 vol (nibbles 0-15)            |
| `0x9001`          | `AUDIO_PSG3_CTRL`  | `0x0VWO` - PSG3 vol V, wave enable W                              |
| `0x9002`          | `AUDIO_PSG_WAVE`   | `0xM123` - M = PSG2 combine mode; 1/2/3 = 2-bit wave for ch 0/1/2 |
| `0x9003`          | `AUDIO_PSG_DUTY`   | `0x0123` - 4-bit duty / saw flip per channel                      |
| `0x9004`          | `AUDIO_PSG_TONE0`  | PSG1 period                                                       |
| `0x9005`          | `AUDIO_PSG_TONE1`  | PSG2 period                                                       |
| `0x9006`          | `AUDIO_PSG_TONE2`  | PSG3 period                                                       |
| `0x9010`-`0x9017` | `AUDIO_WAVETABLE`  | 8 words = 32 x 4-bit steps for PSG3                               |

**Waveform codes** (2-bit): `0` square, `1` saw, `2` noise.

**Combine mode** (top nibble of `$9002`, PSG2 with PSG3):  
`0` off, `1` AND, `2` NAND, `3` OR, `4` NOR, `5` XOR, `6` XNOR.

**Period formula** (NuPy folds this at compile time for constants):

```text
period = round(CHIP_CLOCK_HZ / (64 * freq_Hz))   # clamp 1...65535; 0 = silence
```

Use `note_to_period("A4")` / `freq_to_period(440)` in NuPy, or write tone registers directly.

---

## Interrupts

Control block in **VOLATILE**:

| Address    | Role                                  |
|------------|---------------------------------------|
| `0x00F0`   | Pending bits                          |
| `0x00F1`   | Mask bits                             |
| `0x00F2`   | Status (pending & mask)               |
| `0x00F3`   | Timer reload                          |
| `0x00F4`   | Timer count-down                      |
| `0x00F8+n` | Vector for source *n* (PC to jump to) |

**Sources** (bit index):

| Index | Name         | Raised when                                   |
|-------|--------------|-----------------------------------------------|
| 0     | `IRQ_VBLANK` | Frame present (`FLP` / `frameBlit`)           |
| 1     | `IRQ_INPUT`  | Input signature change (keys / stick / mouse) |
| 2     | `IRQ_TIMER`  | Hardware timer reaches 0                      |
| 3     | `IRQ_SOFT`   | `SWI` / `softwareInterrupt()`                 |
| 4-7   | reserved     |                                               |

**Behavior**

- Global enable: `SEI` / `CLI` (`enableInterrupts` / `disableInterrupts`).
- On service: CPU clears the source bit, pushes PC and status, clears IE, sets `in_isr`, jumps to vector.
- `RTI` / `returnFromInterrupt()` restores status and PC.
- Vector `0` means "no handler" - pending bit is cleared and ignored.
- Timer: write nonzero reload; host decrements count each poll; on zero, raises `IRQ_TIMER` and reloads. Reload `0` disarms the timer.

Stack for calls and IRQs lives in VOLATILE between `STACK_LIMIT` (`0x7000`) and `STACK_TOP` (`0x7F00`). Boot code should `setStackPointer(0x7F00)`.

---

## Input

Host writes these **VOLATILE** ports once per frame (before the CPU instruction budget).

| Address           | Contents                                                             |
|-------------------|----------------------------------------------------------------------|
| `0x8000`-`0x800E` | Keyboard slots; primary character is `$8000`                         |
| `0x800F`          | Reserved / terminator                                                |
| `0x8010`          | Joystick: high byte X, low byte Y; `0x7F` = center (`0x00`...`0xFE`) |
| `0x8011`          | Digital buttons (numpad): bit0-3                                     |
| `0x8012`          | Reserved (written 0)                                                 |
| `0x8013`          | Mouse X (logical 0-319)                                              |
| `0x8014`          | Mouse Y (logical 0-239)                                              |
| `0x8015`          | Mouse state: scroll-down / scroll-up / left / right (nibbles)        |

**Keyboard protocol:** the host presents one queued key as a nonzero value on `$8000` for one frame, then zero for one frame. Guest code should read, clear/debounce, and wait for zero before accepting the next key (see `../disk/mk.nupy`).

Special keys: Enter -> `13`, Backspace -> `8`. Printable text comes from the host text-input path (Unicode code points, stored as 16-bit).

**Joystick** is the numeric keypad (8-direction). **Buttons:** KP0, KP/, KP*, KP-.

**Host keys:** F1 toggles the debugger overlay; Escape requests quit.

---

## Disk controller (MMIO)

Ports in **VOLATILE**. Set argument registers, then write `DISK_CMD`.

| Address  | Name           | Role                                          |
|----------|----------------|-----------------------------------------------|
| `0xFFE0` | `DISK_CMD`     | Command (write to fire)                       |
| `0xFFE1` | `DISK_NAMEPTR` | Pointer to 8-word packed filename in VOLATILE |
| `0xFFE2` | `DISK_REGION`  | 0=VOLATILE, 1=DISK, 2=VRAM, 3=NPROG           |
| `0xFFE3` | `DISK_ADDR`    | Address within that region                    |
| `0xFFE4` | `DISK_LEN`     | Word count (SAVE)                             |
| `0xFFE5` | `DISK_RESULT`  | Status / length written by host               |

| `DISK_CMD` | Operation                                       | RESULT                         |
|------------|-------------------------------------------------|--------------------------------|
| `1` STAT   | Size of named file                              | length, or `0xFFFF` if missing |
| `2` LOAD   | Load file -> REGION:ADDR                        | length, or `0xFFFF` on failure |
| `3` SAVE   | Save LEN words from REGION:ADDR under name      | `1` / `0`                      |
| `4` DELETE | Delete named file                               | `1` / `0`                      |
| `5` EXEC   | Load file into NPROG and jump (does not return) | -                              |

Filename packing matches `writeString`: up to 8 words, one character code per word (low byte), stopped at the first zero/space word.

### On-disk image layout (`.nkg` / `disk` region)

```text
word 0          magic      = 0x4E4B ("NK")
word 1          version    = 1
word 2          fileCount
word 3, 4       next free data offset (hi, lo) - absolute word index in disk
words 5..2052   directory: 128 entries x 16 words
words 2053..    file data (append-only free pointer)
```

**Directory entry** (16 words):

| Words | Field                    |
|-------|--------------------------|
| 0-7   | Name (packed chars)      |
| 8     | Type                     |
| 9     | Flags                    |
| 10-11 | Data offset (hi, lo)     |
| 12-13 | Length in words (hi, lo) |
| 14-15 | Reserved                 |

**Types:** `0` program, `1` graphics, `2` font, `3` data, `4` other.  
**Flags:** `0x0001` bootable, `0x0002` read-only.

A free slot has all name words zero. Deleted/shrunk space is not reclaimed in the current allocator (new data always appends at the free pointer).

Host tools: `tools/mkdisk.py` builds images; `--disk file.nkg` boots the bootable entry into NPROG.

---

## SYSROM

Read-only. Built at startup from `font_std.h` + `charmap.h` unless `--sysrom` / `--no-sysres` is used.

| Range             | Contents                                                                                                  |
|-------------------|-----------------------------------------------------------------------------------------------------------|
| `0` ... `2047`    | Font: 256 tiles x 8 rows; 1 word/row; bit7 = leftmost pixel of the row                                    |
| `2048` ... `2175` | ASCII -> tile keymap (codes 0-127). `0xFFFF` = no glyph. Low 8 bits = tile index; bit 8 = horizontal flip |

Tiles are 8x8. See `sysres_gen.h` and the text drawing helpers in `../disk/mk.nupy`.

---

## Frame loop (host)

Approximate per-frame sequence:

1. Inject entropy into `$80FE` / `$80FF`.
2. Capture input -> keyboard / stick / mouse ports.
3. Update hardware flags (`$FD` frame counter, `$FE`/`$FF` halt/active).
4. Run up to **200,000** instructions (or until `FLP` / halt).
5. Refill audio buffer (Afterburner II).
6. Pace to ~60 Hz.

`FLP` / `frameBlit()` presents the composed November frame and raises `IRQ_VBLANK`. Guest programs that need stable timing should prefer `wait(frames)` (based on `FRC_`) over busy loops.

---

## Quick reference - NuPy ⬌ hardware

| Goal                       | NuPy                                                                               |
|----------------------------|------------------------------------------------------------------------------------|
| Pixel / line               | `setPixel`, `drawLine`, `getPixel`                                                 |
| Present frame              | `frameBlit()`                                                                      |
| Wait N frames              | `wait(n)`                                                                          |
| Palette / sprites / scroll | `setMasterColor`, `setActivePalette`, `setSprite*`, `setSplitScreen`, `setHScroll` |
| Sound                      | `setPSG*`, `note_to_period("C4")`                                                  |
| IRQs                       | `enableInterrupts`, `setIrqVector`, `setIrqMask`, `setTimer`, ...                  |
| Disk file ops              | Low-level: write name buffer + `DISK_*` ports; or use a small helper library       |
| Font from SYSROM           | `read("SYSROM", tile*8 + row)`                                                     |

Constants for VRAM layout and audio ports are mirrored in `tools/nupy.py` for editor convenience.