# Nupy-related stuff to ease the linter

VRAM_, DISK_, VOLATILE_, NPROG_, ROM_ = ["VRAM", "DISK", "VOLATILE", "NPROG", "ROM"]
SYSROM_ = "SYSROM"

FRC_ = 0

# --- November Chip VRAM Pointers ---
BITPLANE_BASE       = 0
PLANE_SIZE          = 4800
SPRITE_PIXELS_BASE  = 28800
SPRITE_ATTRS_BASE   = 32896
SPRITE_PAL_BASE     = 33024
MASTER_PALETTE_BASE = 33280
ACTIVE_PALETTE_BASE = 37376
SPLIT_ENABLE        = 37440
SPLIT_AXIS          = 37441
SPLIT_POS           = 37442
HSCROLL_A           = 37443
HSCROLL_B           = 37444

# --- Existing Intrinsic Stubs ---
def displayLoop(): pass
def halt(): pass
def shutdown(): pass
def cls(): pass
def frameBlit(): pass
def wait(frames: int):
    """
    Wait until the hardware frame counter (FRC_) has advanced by `frames`.
    Each iteration yields with frameBlit so the host advances display and audio.
    Prefer this over a pure software counter when you need stable note/effect
    durations under variable CPU load.
    """
    pass
def setPixel(x, y, color): pass
def drawLine(x0, y0, x1, y1, color): pass
def getPixel(x, y): return 0
def write(region: str, address: int, value: int): pass
def read(region: str, address: int) -> int: return 0
def writeArray(region: str, address: int, array: list): pass
def readArray(region: str, address: int, length: int): pass
def writeString(region: str, address: int, string: str): pass
def readString(region: str, address: int, length: int) -> str: return ""

# --- November Chip Helper Functions ---

def setMasterColor(index, r, g, b):
    """Packs 8-bit RGB into 12-bit hardware format (4 bits/channel)."""
    pass

def setActivePalette(slot, master_index):
    """Maps an on-screen color (0-63) to a master palette entry (0-4095)."""
    pass

def writeBitplaneWord(plane, word_idx, val):
    """Writes 16 packed pixels to a specific hardware bitplane (0-5)."""
    pass

def setSpritePos(sprite_id, x, y):
    """Sets the screen coordinates of a hardware sprite."""
    pass

def setSpriteAttr(sprite_id, flags, scale_x, scale_y, rot, skew):
    """
    Configures sprite hardware attributes.
    flags: bit0=en, bit1=priority, bit2=flip_h, bit3=flip_v
    scale_x/y: Q8.8 fixed point (256 = 1.0)
    rot/skew: degrees
    """
    pass

def setSpriteColor(sprite_id, palette_index, master_index):
    """Points one of a sprite's 16 local colors to the master palette."""
    pass

def setSplitScreen(enabled, axis, pos):
    """
    Enables hardware screen split. 
    axis: 0 = horizontal split, 1 = vertical split.
    pos: row (0-239) or column (0-319).
    """
    pass

def setHScroll(zone_a, zone_b):
    """Sets horizontal scroll per zone, applied via hardware array roll."""
    pass

# --- Afterburner II Audio IO Base Addresses ---
AUDIO_PSG_VOL012   = 0x9000
AUDIO_PSG3_CTRL    = 0x9001
AUDIO_PSG_WAVE     = 0x9002
AUDIO_PSG_DUTY     = 0x9003
AUDIO_PSG_TONE0    = 0x9004
AUDIO_PSG_TONE1    = 0x9005
AUDIO_PSG_TONE2    = 0x9006
AUDIO_WAVETABLE    = 0x9010

# Afterburner II clock (kept in sync with afterburnerII.py / compile.py)
CHIP_CLOCK_HZ = 4_000_000

def note_to_freq(note: str) -> float:
    """
    Convert a musical note name to frequency in Hz (A4 = 440).
    Accepts forms like "C4", "C#5", "Bb3", "A#2".
    In compiled code this folds to a constant; prefer note_to_period for tone registers.
    """
    notes = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
    flats = {"DB": "C#", "EB": "D#", "GB": "F#", "AB": "G#", "BB": "A#"}
    s = note.strip().upper()
    if len(s) < 2:
        raise ValueError(f"invalid note name '{note}'")
    if s[1] in "#B":
        base = s[:2]
        if base in flats:
            base = flats[base]
        octave_str = s[2:]
    else:
        base = s[0]
        octave_str = s[1:]
    if not octave_str or not octave_str.lstrip("-").isdigit():
        raise ValueError(f"invalid note name '{note}' (missing/invalid octave)")
    try:
        octave = int(octave_str)
        note_index = notes.index(base)
    except (ValueError, IndexError):
        raise ValueError(f"invalid note name '{note}'")
    return 440.0 * (2.0 ** ((note_index - 9) / 12.0 + (octave - 4)))

def freq_to_period(freq: float) -> int:
    """
    Convert frequency (Hz) to Afterburner II PSG tone period.
    Formula: period = round(CHIP_CLOCK_HZ / (64 * freq)), clamped to 1..65535.
    period 0 silences the channel. Compiles to a raw immediate when freq is constant.
    """
    if freq <= 0:
        return 0
    period = round(CHIP_CLOCK_HZ / (64.0 * freq))
    return max(1, min(65535, period))

def note_to_period(note: str) -> int:
    """
    Convert a note name (e.g. "A4", "C#3") directly to an Afterburner II tone period.
    Equivalent to freq_to_period(note_to_freq(note)). When the argument is a
    string literal, the compiler folds this to a single immediate in the .nu
    assembly (no runtime calculation).
    """
    return freq_to_period(note_to_freq(note))

def setPSGVolume(master: int, psg1: int, psg2: int):
    """Sets Master volume (0-15) and PSG1/PSG2 volumes (0-15)."""
    pass

def setPSG3Control(volume: int, wave_enable: int):
    """Configures PSG3 volume and wavetable enable flag (0/1). Low nibble is reserved/unused."""
    pass

def setPSGWaveform(w0: int, w1: int, w2: int, bitwise_mode: int = 0):
    """
    Sets 2-bit waveform (0=square, 1=saw, 2=noise) for channels 0..2.
    bitwise_mode: PSG2-combined-with-PSG3 mode, stored in the top nibble of $9002:
    0=off, 1=AND, 2=NAND, 3=OR, 4=NOR, 5=XOR, 6=XNOR.
    """
    pass

def setPSGDuty(d0: int, d1: int, d2: int):
    """Sets 4-bit duty cycle / saw flip for channels 0..2."""
    pass

def setPSGTone(channel: int, period: int):
    """Sets the 16-bit tone period for PSG channel 0, 1, or 2."""
    pass

def setPSG3Wavetable(index: int, word_val: int):
    """Writes a 16-bit word (4 nibbles = 4 waveform steps) into PSG3 Wavetable RAM (0..7)."""
    pass

# --- Interrupts ---
def enableInterrupts(): pass
def disableInterrupts(): pass
def returnFromInterrupt(): pass
def softwareInterrupt(): pass
def push(x: int): pass
def pop(x: int): pass
def setStackPointer(x: int): pass
def setIrqVector(x: int, y: int): pass
def setIrqMask(x: int): pass
def setTimer(x: int): pass
def clearIrq(x: int): pass

# --- Disk ---
def execProgram(name: str): pass
def diskSave(name: str, region: int, addr: int, length: int): pass
def diskDelete(name: str): pass
def diskLoad(name: str, region: int, addr: int): pass

# --- Hardware ---
ENT1_ = 0x80FE
ENT2_ = 0x80FF