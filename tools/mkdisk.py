# mkdisk.py - packs a directory of assets/programs into a .nkg disk image
# for NuevoAuto's Memory disk filesystem (see DISK_* constants in memory.h -
# every constant below must stay in sync with that file).
#
# Usage:
#   python mkdisk.py <src_dir> <output.nkg> [--boot NAME]
#
# src_dir is scanned non-recursively. Each file becomes one disk entry:
#   *.nupy   compiled to binary (via compile.py) and packed as a program
#   *.nub    already-compiled binary, packed as a program as-is
#   *.ncp    SKIPPED - chip-owned default state (e.g. colors.ncp), lives in
#            emures/ now, not on disk
#   anything else   packed verbatim as raw data (2 bytes -> 1 word, little
#            endian; an odd trailing byte is padded with 0)
#
# The entry name is the file's stem, so mk.nupy and mk.nub would collide -
# don't have both for the same program in one src_dir.
#
# --boot NAME marks that entry (by stem, case-insensitive) as the one the
# machine boots into. If omitted and exactly one program-type (.nupy/.nub)
# file is present, it's marked bootable automatically.

import struct
import sys
from pathlib import Path

from compile import compile_source_binary, CompileError

# ---------------------------------------------------------------------------
# Layout constants - mirror memory.h exactly
# ---------------------------------------------------------------------------
DISK_MAGIC = 0x4E4B  # "NK"
DISK_VERSION = 1
DISK_HEADER_WORDS = 5          # magic, version, fileCount, freeOffsetHi, freeOffsetLo
DISK_MAX_FILES = 128
DISK_NAME_WORDS = 8
DISK_ENTRY_WORDS = 16          # name(8) type flags offHi offLo lenHi lenLo x2 reserved
DISK_DIR_START = DISK_HEADER_WORDS
DISK_DATA_START = DISK_DIR_START + DISK_MAX_FILES * DISK_ENTRY_WORDS

DISK_FLAG_BOOTABLE = 0x0001
DISK_FLAG_READONLY = 0x0002

TYPE_PROGRAM = 0
TYPE_GRAPHICS = 1
TYPE_FONT = 2
TYPE_DATA = 3
TYPE_OTHER = 4

SKIP_EXTENSIONS = {".ncp"}
PROGRAM_EXTENSIONS = {".nupy", ".nub"}


class DiskBuildError(Exception):
    pass


class DiskEntry:
    def __init__(self, name, type_, flags, words):
        self.name = name
        self.type = type_
        self.flags = flags
        self.words = words        # list[int], not yet placed
        self.offset = None        # filled in once placed


def bytes_to_words(data: bytes):
    """Little-endian byte stream -> list of 16-bit words, zero-padding an
    odd trailing byte."""
    if len(data) % 2 == 1:
        data = data + b"\x00"
    return list(struct.unpack(f"<{len(data)//2}H", data))


def load_entries(src_dir: Path):
    entries = []
    program_names = []

    for path in sorted(src_dir.iterdir()):
        if not path.is_file():
            continue
        ext = path.suffix.lower()

        if ext in SKIP_EXTENSIONS:
            print(f"skip  {path.name}  (chip-owned, not a disk asset)")
            continue

        name = path.stem
        if not name or len(name) > DISK_NAME_WORDS:
            raise DiskBuildError(
                f"'{path.name}': entry name '{name}' must be 1-{DISK_NAME_WORDS} "
                "characters (rename the file)"
            )
        if any(ord(c) > 255 or c == "\x00" for c in name):
            raise DiskBuildError(f"'{path.name}': name has unsupported characters")
        if any(e.name == name for e in entries):
            raise DiskBuildError(f"duplicate entry name '{name}' (from {path.name})")

        if ext == ".nupy":
            try:
                data = compile_source_binary(path.read_text())
            except CompileError as e:
                raise DiskBuildError(f"'{path.name}': compile error: {e}")
            words = bytes_to_words(data)
            entries.append(DiskEntry(name, TYPE_PROGRAM, 0, words))
            program_names.append(name)
            print(f"pack  {path.name:30s} -> program '{name}' ({len(words)} words)")

        elif ext == ".nub":
            words = bytes_to_words(path.read_bytes())
            entries.append(DiskEntry(name, TYPE_PROGRAM, 0, words))
            program_names.append(name)
            print(f"pack  {path.name:30s} -> program '{name}' ({len(words)} words)")

        else:
            words = bytes_to_words(path.read_bytes())
            entries.append(DiskEntry(name, TYPE_OTHER, DISK_FLAG_READONLY, words))
            print(f"pack  {path.name:30s} -> data '{name}' ({len(words)} words)")

    return entries, program_names


def mark_boot(entries, program_names, boot_name):
    if boot_name is not None:
        matches = [e for e in entries if e.name.lower() == boot_name.lower()]
        if not matches:
            raise DiskBuildError(f"--boot '{boot_name}' does not match any packed entry")
        matches[0].flags |= DISK_FLAG_BOOTABLE
        print(f"boot  '{matches[0].name}'")
        return

    if len(program_names) == 1:
        for e in entries:
            if e.name == program_names[0]:
                e.flags |= DISK_FLAG_BOOTABLE
                print(f"boot  '{e.name}'  (auto-selected, only program on disk)")
                return

    if len(program_names) == 0:
        print("warning: no program files packed - disk has no bootable entry")
    else:
        print(
            "warning: multiple program files packed and no --boot given - "
            "disk has no bootable entry: " + ", ".join(program_names)
        )


def build_image(entries):
    if len(entries) > DISK_MAX_FILES:
        raise DiskBuildError(f"too many files ({len(entries)}), disk holds at most {DISK_MAX_FILES}")

    data_words = []
    for e in entries:
        e.offset = DISK_DATA_START + len(data_words)
        data_words.extend(e.words)

    free_offset = DISK_DATA_START + len(data_words)

    header = [
        DISK_MAGIC,
        DISK_VERSION,
        len(entries),
        (free_offset >> 16) & 0xFFFF,
        free_offset & 0xFFFF,
    ]

    dir_table = [0] * (DISK_MAX_FILES * DISK_ENTRY_WORDS)
    for i, e in enumerate(entries):
        base = i * DISK_ENTRY_WORDS
        for j, ch in enumerate(e.name):
            dir_table[base + j] = ord(ch)
        length = len(e.words)
        dir_table[base + 8] = e.type
        dir_table[base + 9] = e.flags
        dir_table[base + 10] = (e.offset >> 16) & 0xFFFF
        dir_table[base + 11] = e.offset & 0xFFFF
        dir_table[base + 12] = (length >> 16) & 0xFFFF
        dir_table[base + 13] = length & 0xFFFF
        # words base+14, base+15 stay reserved/zero

    words = header + dir_table + data_words
    return struct.pack(f"<{len(words)}H", *words)


def write_disk(src_dir: str, out_path: str, boot_name):
    src = Path(src_dir)
    if not src.is_dir():
        raise DiskBuildError(f"'{src_dir}' is not a directory")

    entries, program_names = load_entries(src)
    mark_boot(entries, program_names, boot_name)
    image = build_image(entries)

    out = Path(out_path)
    out.write_bytes(image)
    print(f"wrote {out_path}  ({len(image)} bytes, {len(entries)} files)")
    return out_path


if __name__ == "__main__":
    args = sys.argv[1:]
    boot_name = None
    if "--boot" in args:
        i = args.index("--boot")
        if i + 1 >= len(args):
            print("--boot requires a name")
            raise SystemExit(1)
        boot_name = args[i + 1]
        del args[i:i + 2]

    if len(args) != 2:
        print("usage: python mkdisk.py <src_dir> <output.nkg> [--boot NAME]")
        raise SystemExit(1)

    try:
        write_disk(args[0], args[1], boot_name)
    except DiskBuildError as e:
        print(f"error: {e}")
        raise SystemExit(1)
