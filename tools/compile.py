# NuevoAuto program compiler/parser (16-Bit Updated)
# Compiles a subset of Python into ".nu" text assembly for the NACPU

import ast
import math
import sys
from commentgen import generate_comment


# Afterburner II audio clock (kept in sync with afterburnerII.py)
CHIP_CLOCK_HZ = 4_000_000

def _note_to_freq(note: str) -> float:
    """Convert a note name (e.g. 'A4', 'C#5', 'Bb3') to frequency in Hz (A4 = 440)."""
    notes = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
    flats = {"DB": "C#", "EB": "D#", "GB": "F#", "AB": "G#", "BB": "A#"}
    s = note.strip().upper()
    if len(s) < 2:
        raise CompileError(f"invalid note name '{note}'")
    if s[1] in "#B":
        base = s[:2]
        if base in flats:
            base = flats[base]
        octave_str = s[2:]
    else:
        base = s[0]
        octave_str = s[1:]
    if not octave_str or not octave_str.lstrip("-").isdigit():
        raise CompileError(f"invalid note name '{note}' (missing/invalid octave)")
    try:
        octave = int(octave_str)
        note_index = notes.index(base)
    except (ValueError, IndexError):
        raise CompileError(f"invalid note name '{note}'")
    # A4 = index 9, octave 4 → 440 Hz
    return 440.0 * (2.0 ** ((note_index - 9) / 12.0 + (octave - 4)))

def _freq_to_period(freq: float) -> int:
    """Convert frequency (Hz) to Afterburner II tone period.
    period = CHIP_CLOCK / (64 * freq), rounded, clamped to 1..65535 (0 = silence)."""
    if freq <= 0:
        return 0
    period = round(CHIP_CLOCK_HZ / (64.0 * freq))
    return max(1, min(65535, period))

def _note_to_period(note: str) -> int:
    return _freq_to_period(_note_to_freq(note))


# Canonical opcode order - index in this list IS the binary opcode word.
# Must stay byte-for-byte in sync with OPCODES in NACPU.h / python_ref/cpu.py.
OPCODES = [
    "NOP", "MOV", "SMV", "VMV", "LDM", "LOD", "SWP",
    "ADD", "ADC", "SUB", "SBB", "INC", "DEC", "MUL", "DIV",
    "AND", "OR", "XOR", "NOT", "NEG",
    "SHL", "SAL", "SHR", "SAR",
    "JMP", "JE", "JNE", "JZ", "JNZ", "JL", "JLE", "JG", "JGE",
    "JB", "JBE", "JA", "JAE",
    "CAL", "RET", "FOR", "BRK", "PRT",
    "LSM", "LVM", "LNP", "SNP", "FLP",
    "MOVI", "SMVI", "VMVI", "LDMI", "LSMI", "LVMI", "LNPI", "SNPI",
    "LBM", "LBMI",
    "PSH", "POP", "LSP", "SSP", "SEI", "CLI", "RTI", "SWI",
]
OPCODE_INDEX = {name: i for i, name in enumerate(OPCODES)}

# Kept in sync with cpu.py 16-bit architecture
OPERAND_COUNTS = {
    "NOP": 0, "MOV": 2, "SMV": 2, "VMV": 2, "LDM": 2, "LOD": 2, "SWP": 2,
    "ADD": 3, "ADC": 3, "SUB": 3, "SBB": 3, "INC": 1, "DEC": 1, "MUL": 3, "DIV": 3,
    "AND": 3, "OR": 3, "XOR": 3, "NOT": 2, "NEG": 2, "SHL": 3, "SAL": 3, "SHR": 3, "SAR": 3,
    # Control transfers now take a single 16-bit absolute address word
    "JMP": 1, "JE": 3, "JNE": 3, "JZ": 2, "JNZ": 2, "JL": 3, "JLE": 3, "JG": 3, "JGE": 3,
    "JB": 3, "JBE": 3, "JA": 3, "JAE": 3,
    "CAL": 1, "RET": 0, "FOR": 3, "BRK": 0, "PRT": 1,
    "LSM": 2, "LVM": 2, "LNP": 2, "SNP": 2,
    "FLP": 0,
    "MOVI": 2, "SMVI": 2, "VMVI": 2,
    "LDMI": 2, "LSMI": 2, "LVMI": 2, "LNPI": 2, "SNPI": 2,
    "LBM": 2, "LBMI": 2,
    # Stack / interrupt
    "PSH": 1, "POP": 1, "LSP": 1, "SSP": 1,
    "SEI": 0, "CLI": 0, "RTI": 0, "SWI": 0,
}

assert set(OPCODES) == set(OPERAND_COUNTS), (
    "OPCODES and OPERAND_COUNTS have drifted apart - "
    f"only in OPCODES: {set(OPCODES) - set(OPERAND_COUNTS)}, "
    f"only in OPERAND_COUNTS: {set(OPERAND_COUNTS) - set(OPCODES)}"
)

# GPU command port - kept in sync with cpu.py. setPixel/drawLine/getPixel no
# longer compile to their own opcodes; they poke these VOLATILE addresses
# via ordinary MOV/LDM, and writing GPU_CMD is what triggers the op.
GPU_CMD    = 0xFFF0
GPU_X0     = 0xFFF1
GPU_Y0     = 0xFFF2
GPU_X1     = 0xFFF3
GPU_Y1     = 0xFFF4
GPU_COLOR  = 0xFFF5
GPU_RESULT = 0xFFF6

GPU_CMD_SETPIXEL = 1
GPU_CMD_GETPIXEL = 2
GPU_CMD_DRAWLINE = 3

# Disk controller port - mirrors the GPU_CMD pattern above, kept in sync
# with NACPU.h. DISK_NAMEPTR points at an 8-word packed filename buffer in
# VOLATILE; the compiler always uses DISK_NAME_BUF for that (reserved,
# not otherwise addressable from nupy source).
DISK_CMD      = 0xFFE0
DISK_NAMEPTR  = 0xFFE1
DISK_REGION   = 0xFFE2
DISK_ADDR     = 0xFFE3
DISK_LEN      = 0xFFE4
DISK_RESULT   = 0xFFE5
DISK_NAME_BUF = 0xFFD0  # 8 reserved words: 0xFFD0-0xFFD7

DISK_CMD_STAT   = 1
DISK_CMD_LOAD   = 2
DISK_CMD_SAVE   = 3
DISK_CMD_DELETE = 4
DISK_CMD_EXEC   = 5

# Region codes for diskLoad/diskSave - matches NACPU's diskRegionName().
# NPROG is intentionally excluded here: writing another program's code into
# your own NPROG out from under the running fetch loop isn't something
# these helpers support - use execProgram() to run another program.
DISK_REGION_CODE = {"VOLATILE": 0, "DISK": 1, "VRAM": 2}

# November Chip VRAM map (kept in sync with nupy.py)
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

# IRQ / stack memory map (kept in sync with cpu.py)
STACK_TOP        = 0x7F00
IRQ_PENDING      = 0x00F0
IRQ_MASK         = 0x00F1
IRQ_STATUS       = 0x00F2
IRQ_TIMER_RELOAD = 0x00F3
IRQ_TIMER_COUNT  = 0x00F4
IRQ_VECTOR_BASE  = 0x00F8

IRQ_VBLANK = 0
IRQ_INPUT  = 1
IRQ_TIMER  = 2
IRQ_SOFT   = 3

# Register spill / save-slot base. Must sit strictly above the IRQ control
# block (0x00F0..0x00FF) and the low hardware-flag ports (0x00FD..0x00FF).
# Starting at 0 caused later functions (e.g. do_peek) to overwrite
# IRQ_PENDING / IRQ_MASK / IRQ_STATUS / timer / vectors, which made the
# kernel crash or corrupt memory when interrupts were enabled.
SAVE_SLOT_BASE   = 0x0100

NUM_VAR_REGS = 11          # R0-R10
TEMP_REGS = [11, 12, 13, 14]
RET_REG = 15

MEM_REGIONS = {
    "VOLATILE_": ("VOLATILE", "MOV", "LDM", "MOVI", "LDMI"),
    "DISK_":     ("DISK",     "SMV", "LSM", "SMVI", "LSMI"),
    "VRAM_":     ("VRAM",     "VMV", "LVM", "VMVI", "LVMI"),
    "NPROG_":    ("NPROG",    "SNP", "LNP", "SNPI", "LNPI"),
    "SYSROM_":   ("SYSROM",   None,  "LBM", None,   "LBMI"),
}

CMP_JUMP = {
    ast.Eq: ( "JE",  "JNE"), ast.NotEq: ( "JNE",  "JE"),
    ast.Lt: ( "JL",  "JGE"), ast.LtE: ( "JLE",  "JG"),
    ast.Gt: ( "JG",  "JLE"), ast.GtE: ( "JGE",  "JL"),
}
BINOP = {
    ast.Add:  "ADD", ast.Sub:  "SUB", ast.Mult:  "MUL", ast.FloorDiv:  "DIV",
    ast.BitAnd:  "AND", ast.BitOr:  "OR", ast.BitXor:  "XOR",
    ast.LShift:  "SHL", ast.RShift:  "SHR",
}

SHIFT_OPS = {"SHL", "SAL", "SHR", "SAR"}

class CompileError(Exception):
    pass

def _reg(n): return "reg", n
def _imm(v):
    if not (-32768 <= v <= 65535): raise CompileError(f"immediate {v} out of 16-bit range")
    return "imm", v & 0xFFFF
def _lbl(name): return "label", name


class Scope:
    def __init__(self, name):
        self.name = name
        self.vars = {}
        self.next_var = 0
        self.temp_sp = 0

    def var_reg(self, name):
        if name not in self.vars:
            if self.next_var >= NUM_VAR_REGS:
                raise CompileError(f"'{self.name}' uses more than {NUM_VAR_REGS} variables")
            self.vars[name] = self.next_var
            self.next_var += 1
        return self.vars[name]

    def alloc_temp(self):
        if self.temp_sp >= len(TEMP_REGS):
            raise CompileError("expression nested too deeply (max 4 levels)")
        r = TEMP_REGS[self.temp_sp]
        self.temp_sp += 1
        return r

    def free_temp(self):
        self.temp_sp -= 1

class FuncInfo:
    def __init__(self, node, start_label):
        self.node = node
        self.start_label = start_label
        self.params = [a.arg for a in node.args.args]
        if len(self.params) > len(TEMP_REGS):
            raise CompileError(f"function '{node.name}' has more than {len(TEMP_REGS)} params")


def _is_read_string_call(node):
    return (
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Name)
            and node.func.id == "readString"
    )


def _match_string_literal_compare(node):
    """Detect `readString(region, addr, length) == "literal"` (or the
    reverse order, or !=). Returns (call_node, literal_str, is_eq) or
    None if `node` isn't that shape."""
    if not (isinstance(node, ast.Compare) and len(node.ops) == 1 and len(node.comparators) == 1):
        return None
    op = node.ops[0]
    if not isinstance(op, (ast.Eq, ast.NotEq)):
        return None
    left, right = node.left, node.comparators[0]
    for call, lit in ((left, right), (right, left)):
        if (isinstance(call, ast.Call) and isinstance(call.func, ast.Name)
                and call.func.id == "readString"
                and isinstance(lit, ast.Constant) and isinstance(lit.value, str)):
            return call, lit.value, isinstance(op, ast.Eq)
    return None


def _free_if_temp(operand, scope):
    if operand[0] == "reg" and operand[1] in TEMP_REGS:
        scope.free_temp()


def _result_reg(src_operand, scope):
    if src_operand[0] == "reg" and src_operand[1] in TEMP_REGS:
        return src_operand[1]
    return scope.alloc_temp()


class Compiler:
    def __init__(self):
        self.instrs = []
        self.label_ctr = 0
        self.funcs = {}
        self.call_stack = []
        self.save_addr = SAVE_SLOT_BASE

    def new_label(self, hint):
        self.label_ctr += 1
        return f"{hint}_{self.label_ctr}"

    def emit(self, mnemonic, *operands):
        self.instrs.append((mnemonic, list(operands)))

    def emit_comment(self, text):
        """Emit a free-form comment line that does not affect code addresses."""
        self.instrs.append(("COMMENT", text))

    def mark(self, label):
        self.instrs.append(("LABEL", label))

    def emit_copy(self, dest_reg, src_reg):
        if dest_reg == src_reg: return
        self.emit("SWP", _reg(dest_reg), _reg(src_reg))

    def emit_copy_preserving_src(self, dest_reg, src_reg):
        """Copy src_reg into dest_reg without touching src_reg. emit_copy
        uses SWP, which also overwrites src_reg - fine when src is about to
        be discarded, but wrong when src is a live variable the caller still
        needs (e.g. the address argument to writeArray/readArray)."""
        if dest_reg == src_reg: return
        self.emit("OR", _reg(dest_reg), _reg(src_reg), _reg(src_reg))

    def _to_reg(self, op, scope):
        return self._imm_to_reg(op, scope) if op[0] == "imm" else op

    ALU3_REGISTER_ONLY = {"ADD", "ADC", "SUB", "SBB", "AND", "OR", "XOR", "MUL", "DIV"}

    def emit_alu3(self, opcode, dest_operand, a_operand, b_operand, scope):
        """Emit a 3-operand ALU instruction (ADD/AND/OR/MUL/...). Unlike
        SHL/SHR, these opcodes always read *every* operand as a register
        index on this CPU - there's no immediate-operand mode - so an
        immediate here (e.g. `_imm(0xFF)`) would be decoded as register
        255 and crash. Any immediate operand is loaded into a scratch
        register first."""
        assert opcode in self.ALU3_REGISTER_ONLY
        a_is_new = a_operand[0] == "imm"
        b_is_new = b_operand[0] == "imm"
        a_reg = self._to_reg(a_operand, scope)
        b_reg = self._to_reg(b_operand, scope)
        self.emit(opcode, dest_operand, a_reg, b_reg)
        if b_is_new: scope.free_temp()
        if a_is_new: scope.free_temp()

    def emit_gpu_command(self, cmd_const, addr_value_pairs, scope):
        """
        addr_value_pairs: [(gpu_addr, operand), ...]. MOVs each operand into
        its VOLATILE register, then MOVs the command word into GPU_CMD last
        (that final write is what triggers the GPU on the host side).
        Frees any temps it had to allocate for immediates along the way.
        """
        reg_ops = [self._to_reg(op, scope) for _, op in addr_value_pairs]
        for (addr, _), r in zip(addr_value_pairs, reg_ops):
            self.emit("MOV", r, _imm(addr))
        cmd_reg = scope.alloc_temp()
        self.emit("LOD", _reg(cmd_reg), _imm(cmd_const))
        self.emit("MOV", _reg(cmd_reg), _imm(GPU_CMD))
        scope.free_temp()
        for r in reg_ops:
            _free_if_temp(r, scope)

    def _emit_disk_name(self, name, scope):
        """Pack `name` (a compile-time string, max 8 chars) into the reserved
        VOLATILE scratch buffer at DISK_NAME_BUF and point DISK_NAMEPTR at
        it. Shared by every disk-controller intrinsic (execProgram,
        diskStat/Load/Save/Delete) - each just needs this plus setting
        whichever of DISK_REGION/DISK_ADDR/DISK_LEN/DISK_CMD it needs."""
        if len(name) > 8:
            raise CompileError(f"disk file name '{name}' is longer than 8 characters")
        if not name:
            raise CompileError("disk file name cannot be empty")
        vals = [ord(c) & 0xFF for c in name] + [0] * (8 - len(name))
        for i, v in enumerate(vals):
            t = scope.alloc_temp()
            self.emit("LOD", _reg(t), _imm(v))
            self.emit("MOV", _reg(t), _imm(DISK_NAME_BUF + i))
            scope.free_temp()
        t = scope.alloc_temp()
        self.emit("LOD", _reg(t), _imm(DISK_NAME_BUF))
        self.emit("MOV", _reg(t), _imm(DISK_NAMEPTR))
        scope.free_temp()

    def _emit_read_string_source(self, node, scope):
        """Compile readString(region, addr, length) as a source string.

        A runtime string is represented by its source memory region, address,
        and compile-time length. This helper returns (read_ind, addr_reg,
        length), with addr_reg owned by the caller and safe to increment.
        """
        if not _is_read_string_call(node):
            raise CompileError("expected readString(...)")
        if len(node.args) != 3:
            raise CompileError("readString(region, addr, length) takes exactly 3 arguments")
        region_arg, addr_arg, len_arg = node.args
        if not (isinstance(region_arg, ast.Constant) and isinstance(region_arg.value, str)):
            raise CompileError("readString region must be a string literal")
        if not (isinstance(len_arg, ast.Constant) and isinstance(len_arg.value, int)):
            raise CompileError("readString length must be an integer constant")
        length = len_arg.value
        if length < 0:
            raise CompileError("readString length cannot be negative")

        mem_name = region_arg.value + "_"
        if mem_name not in MEM_REGIONS:
            raise CompileError(f"unknown region '{region_arg.value}'")
        _, _, _, _, read_ind = MEM_REGIONS[mem_name]

        addr_op = self.compile_expr(addr_arg, scope)
        if addr_op[0] == "imm":
            addr_reg = scope.alloc_temp()
            self.emit("LOD", _reg(addr_reg), addr_op)
        elif addr_op[0] == "reg" and addr_op[1] in TEMP_REGS:
            addr_reg = addr_op[1]
        else:
            addr_reg = scope.alloc_temp()
            self.emit_copy_preserving_src(addr_reg, addr_op[1])

        return read_ind, _reg(addr_reg), length, addr_op

    def _emit_disk_name_arg(self, name_node, scope):
        """Pack either a literal or readString(...) into DISK_NAME_BUF."""
        if isinstance(name_node, ast.Constant) and isinstance(name_node.value, str):
            self._emit_disk_name(name_node.value, scope)
            return

        if not _is_read_string_call(name_node):
            raise CompileError(
                "disk name must be a string literal or readString(region, addr, length)"
            )

        read_ind, addr_op, length, original_addr = self._emit_read_string_source(
            name_node, scope
        )
        if not (1 <= length <= 8):
            _free_if_temp(addr_op, scope)
            raise CompileError("disk name readString length must be 1..8")

        val_reg = scope.alloc_temp()
        for i in range(length):
            self.emit(read_ind, _reg(val_reg), addr_op)
            self.emit_alu3("AND", _reg(val_reg), _reg(val_reg), _imm(0xFF), scope)
            self.emit("MOV", _reg(val_reg), _imm(DISK_NAME_BUF + i))
            if i < length - 1:
                self.emit("INC", addr_op)

        # A short runtime name is padded with zeroes, exactly like a literal.
        for i in range(length, 8):
            self.emit("LOD", _reg(val_reg), _imm(0))
            self.emit("MOV", _reg(val_reg), _imm(DISK_NAME_BUF + i))

        scope.free_temp()  # val_reg
        _free_if_temp(addr_op, scope)

        t = scope.alloc_temp()
        self.emit("LOD", _reg(t), _imm(DISK_NAME_BUF))
        self.emit("MOV", _reg(t), _imm(DISK_NAMEPTR))
        scope.free_temp()

    def _emit_read_string_to_memory(self, node, write_ind,
                                    start_addr_op, scope):
        """Copy readString(...) character data into a destination buffer."""
        read_ind, src_op, length, _ = self._emit_read_string_source(node, scope)

        if start_addr_op[0] == "imm":
            dst_reg = scope.alloc_temp()
            self.emit("LOD", _reg(dst_reg), start_addr_op)
        elif start_addr_op[0] == "reg" and start_addr_op[1] in TEMP_REGS:
            dst_reg = start_addr_op[1]
        else:
            dst_reg = scope.alloc_temp()
            self.emit_copy_preserving_src(dst_reg, start_addr_op[1])

        val_reg = scope.alloc_temp()
        for i in range(length):
            self.emit(read_ind, _reg(val_reg), src_op)
            self.emit_alu3("AND", _reg(val_reg), _reg(val_reg), _imm(0xFF), scope)
            self.emit(write_ind, _reg(val_reg), _reg(dst_reg))
            if i < length - 1:
                self.emit("INC", src_op)
                self.emit("INC", _reg(dst_reg))

        scope.free_temp()  # val_reg
        if dst_reg in TEMP_REGS:
            scope.free_temp()
        _free_if_temp(src_op, scope)

    def _emit_disk_region(self, region_name, scope):
        """Write DISK_REGION_CODE[region_name] into the DISK_REGION port."""
        if region_name not in DISK_REGION_CODE:
            raise CompileError(
                f"disk region must be one of {list(DISK_REGION_CODE)}, got '{region_name}'"
            )
        t = scope.alloc_temp()
        self.emit("LOD", _reg(t), _imm(DISK_REGION_CODE[region_name]))
        self.emit("MOV", _reg(t), _imm(DISK_REGION))
        scope.free_temp()

    def _emit_disk_trigger(self, cmd_const, scope):
        t = scope.alloc_temp()
        self.emit("LOD", _reg(t), _imm(cmd_const))
        self.emit("MOV", _reg(t), _imm(DISK_CMD))
        scope.free_temp()

    def alloc_save_slots(self, count):
        # Spills grow from SAVE_SLOT_BASE upward.  They must never enter the
        # IRQ block (0x00F0-0x00FF) or the stack region near STACK_TOP.
        if self.save_addr + count > STACK_TOP:
            raise CompileError(
                "ran out of VOLATILE address space saving registers "
                f"(need {count} slots starting at 0x{self.save_addr:04X})"
            )
        start = self.save_addr
        self.save_addr += count
        return start

    def compile_module(self, tree):
        func_defs = [n for n in tree.body if isinstance(n, ast.FunctionDef)]
        main_stmts = [n for n in tree.body if not isinstance(n, ast.FunctionDef)]

        # Module-level docstring (if present) as leading comments
        mod_doc = ast.get_docstring(tree)
        if mod_doc:
            self.emit_comment("--- module docstring ---")
            for line in mod_doc.strip().splitlines():
                self.emit_comment(f"  {line.rstrip()}")

        main_label = self.new_label("main")
        if func_defs:
            self.emit("JMP", _lbl(main_label))
            for fn in func_defs:
                start = self.new_label(f"fn_{fn.name}")
                self.funcs[fn.name] = FuncInfo(fn, start)
            self._check_no_recursion(func_defs)
            for fn in func_defs:
                self.compile_function(fn)

        self.mark(main_label)
        self.emit_comment("--- main / entry point ---")
        scope = Scope("<main>")
        for stmt in main_stmts:
            self.compile_stmt(stmt, scope, None, None)
        self.emit("BRK")

    def _check_no_recursion(self, func_defs):
        calls = {}
        for fn in func_defs:
            called = set()
            for node in ast.walk(fn):
                if isinstance(node, ast.Call) and isinstance(node.func, ast.Name):
                    if node.func.id in self.funcs:
                        called.add(node.func.id)
            calls[fn.name] = called

        WHITE, GRAY, BLACK = 0, 1, 2
        color = {name: WHITE for name in calls}

        def visit(name, path):
            color[name] = GRAY
            for callee in calls[name]:
                if color[callee] == GRAY:
                    cycle = " -> ".join(path[path.index(callee):] + [callee])
                    raise CompileError(
                        f"recursive call cycle detected ({cycle})"
                    )
                if color[callee] == WHITE:
                    visit(callee, path + [callee])
            color[name] = BLACK

        for name in calls:
            if color[name] == WHITE:
                visit(name, [name])

    def compile_function(self, fn):
        info = self.funcs[fn.name]
        if fn.name in self.call_stack:
            raise CompileError(f"recursive call involving '{fn.name}' is not supported")
        self.call_stack.append(fn.name)

        self.mark(info.start_label)

        # Emit function header + docstring as assembly comments
        params = ", ".join(info.params)
        self.emit_comment(f"--- def {fn.name}({params}): ---")
        doc = ast.get_docstring(fn)
        if doc:
            for line in doc.strip().splitlines():
                self.emit_comment(f"  {line.rstrip()}")

        scope = Scope(fn.name)
        for i, pname in enumerate(info.params):
            scope.vars[pname] = i
            scope.next_var = max(scope.next_var, i + 1)

        for stmt in fn.body:
            self.compile_stmt(stmt, scope, None, None)

        if not (self.instrs and self.instrs[-1] == ("RET", [])):
            self.emit("RET")
        self.call_stack.pop()

    def compile_stmt(self, node, scope, break_label, continue_label):
        if isinstance(node, ast.Assign):

            if len(node.targets) == 1 and isinstance(node.targets[0], ast.Name):
                if isinstance(node.value, ast.List):
                    if not hasattr(self, 'array_constants'):
                        self.array_constants = {}
                    vals = []
                    for elt in node.value.elts:
                        if not isinstance(elt, ast.Constant) or not isinstance(elt.value, int):
                            raise CompileError("Arrays must only contain integer constants")
                        vals.append(elt.value)
                    self.array_constants[node.targets[0].id] = vals
                    return

            if len(node.targets) != 1:
                raise CompileError("only simple assignment is supported")
            target = node.targets[0]

            if isinstance(target, ast.Name):
                if target.id == "FRC_":
                    raise CompileError("FRC_ is read-only (hardware frame counter)")
                dest = scope.var_reg(target.id)
                self.compile_expr_into(node.value, scope, dest)

            elif isinstance(target, ast.Subscript):
                if not isinstance(target.value, ast.Name):
                    raise CompileError("unsupported memory target")
                mem_name = target.value.id
                if mem_name not in MEM_REGIONS:
                    if hasattr(self, 'array_constants') and mem_name in self.array_constants:
                        raise CompileError("cannot mutate constant arrays")
                    raise CompileError(f"unknown memory region '{mem_name}'")

                slice_node = target.slice
                if hasattr(ast, 'Index') and isinstance(slice_node, ast.Index):
                    slice_node = slice_node.value

                _, write_imm, _, write_ind, _ = MEM_REGIONS[mem_name]

                addr_op = self.compile_expr(slice_node, scope)
                val_op = self.compile_expr(node.value, scope)
                if val_op[0] == "imm":
                    val_reg = scope.alloc_temp()
                    self.emit("LOD", _reg(val_reg), val_op)
                    val_op = _reg(val_reg)

                if addr_op[0] == "imm":
                    self.emit(write_imm, val_op, addr_op)
                else:
                    self.emit(write_ind, val_op, addr_op)

                if val_op[0] == "reg" and val_op[1] in TEMP_REGS: scope.free_temp()
                if addr_op[0] == "reg" and addr_op[1] in TEMP_REGS: scope.free_temp()
            else:
                raise CompileError("unsupported assignment target")

        elif isinstance(node, ast.AugAssign):
            if isinstance(node.target, ast.Name):
                if node.target.id == "FRC_":
                    raise CompileError("FRC_ is read-only (hardware frame counter)")
                dest = scope.var_reg(node.target.id)
                opcode = BINOP.get(type(node.op))
                if opcode is None: raise CompileError(f"unsupported augmented op {node.op}")
                rhs = self.compile_expr(node.value, scope)
                if opcode in SHIFT_OPS:
                    if rhs[0] != "imm":
                        raise CompileError(f"{opcode} amount must be a constant")
                    self.emit(opcode, _reg(dest), _reg(dest), rhs)
                else:
                    if rhs[0] == "imm":
                        rhs = self._imm_to_reg(rhs, scope)
                    self.emit(opcode, _reg(dest), _reg(dest), rhs)
                    if rhs[0] == "reg" and rhs[1] in TEMP_REGS: scope.free_temp()

            elif isinstance(node.target, ast.Subscript):
                if not isinstance(node.target.value, ast.Name):
                    raise CompileError("unsupported memory target")
                mem_name = node.target.value.id
                if mem_name not in MEM_REGIONS:
                    raise CompileError(f"unknown memory region '{mem_name}'")
                _, write_imm, read_imm, write_ind, read_ind = MEM_REGIONS[mem_name]
                if write_imm is None:
                    raise CompileError(f"memory region '{mem_name}' is read-only")

                slice_node = node.target.slice
                if hasattr(ast, 'Index') and isinstance(slice_node, ast.Index):
                    slice_node = slice_node.value

                addr_op = self.compile_expr(slice_node, scope)
                val_reg = scope.alloc_temp()
                if addr_op[0] == "imm":
                    self.emit(read_imm, _reg(val_reg), addr_op)
                else:
                    self.emit(read_ind, _reg(val_reg), addr_op)

                rhs_op = self.compile_expr(node.value, scope)

                opcode = BINOP.get(type(node.op))
                if opcode is None: raise CompileError(f"unsupported augmented op {node.op}")

                if opcode in SHIFT_OPS:
                    if rhs_op[0] != "imm":
                        raise CompileError(f"{opcode} amount must be a constant")
                elif rhs_op[0] == "imm":
                    rhs_reg = scope.alloc_temp()
                    self.emit("LOD", _reg(rhs_reg), rhs_op)
                    rhs_op = _reg(rhs_reg)

                res_reg = scope.alloc_temp()
                self.emit(opcode, _reg(res_reg), _reg(val_reg), rhs_op)
                if addr_op[0] == "imm":
                    self.emit(write_imm, _reg(res_reg), addr_op)
                else:
                    self.emit(write_ind, _reg(res_reg), addr_op)

                if addr_op[0] == "reg" and addr_op[1] in TEMP_REGS: scope.free_temp()
                if rhs_op[0] == "reg" and rhs_op[1] in TEMP_REGS: scope.free_temp()
                if val_reg in TEMP_REGS: scope.free_temp()
                if res_reg in TEMP_REGS: scope.free_temp()
            else:
                raise CompileError("unsupported augmented assignment target")

        elif isinstance(node, ast.Expr):
            # Skip pure string expression statements (docstrings left in the AST body)
            if isinstance(node.value, ast.Constant) and isinstance(node.value.value, str):
                pass
            else:
                self.compile_call_stmt(node.value, scope)
        elif isinstance(node, ast.If):
            self.compile_if(node, scope, break_label, continue_label)
        elif isinstance(node, ast.While):
            self.compile_while(node, scope)
        elif isinstance(node, ast.For):
            self.compile_for(node, scope)
        elif isinstance(node, ast.Break):
            if break_label is None: raise CompileError("'break' outside a loop")
            self.emit("JMP", _lbl(break_label))
        elif isinstance(node, ast.Continue):
            if continue_label is None: raise CompileError("'continue' outside a loop")
            self.emit("JMP", _lbl(continue_label))
        elif isinstance(node, ast.Return):
            if node.value is not None:
                self.compile_expr_into(node.value, scope, RET_REG)
            self.emit("RET")
        elif isinstance(node, ast.Pass):
            self.emit("NOP")
        elif isinstance(node, ast.ImportFrom) or isinstance(node, ast.Import):
            pass
        else:
            raise CompileError(f"unsupported statement: {ast.dump(node)}")

    def compile_call_stmt(self, node, scope):
        if not isinstance(node.func, ast.Call):
            if isinstance(node.func, ast.Name):
                fname = node.func.id

                if fname == "write":
                    if len(node.args) != 3:
                        raise CompileError("write(region, addr, val) takes 3 arguments")
                    region_arg, addr_arg, val_arg = node.args
                    if not (isinstance(region_arg, ast.Constant) and isinstance(region_arg.value, str)):
                        raise CompileError("write region must be a string literal")

                    mem_name = region_arg.value + "_"
                    if mem_name not in MEM_REGIONS:
                        raise CompileError(f"unknown region '{region_arg.value}'")
                    _, write_imm, _, write_ind, _ = MEM_REGIONS[mem_name]

                    addr_op = self.compile_expr(addr_arg, scope)
                    val_op = self.compile_expr(val_arg, scope)

                    if val_op[0] == "imm":
                        val_reg = scope.alloc_temp()
                        self.emit("LOD", _reg(val_reg), val_op)
                        val_op = _reg(val_reg)

                    if addr_op[0] == "imm":
                        self.emit(write_imm, val_op, addr_op)
                    else:
                        self.emit(write_ind, val_op, addr_op)

                    _free_if_temp(val_op, scope)
                    _free_if_temp(addr_op, scope)
                    return

                elif fname == "writeArray":
                    if len(node.args) != 3:
                        raise CompileError("writeArray(region, start_addr, array) takes 3 arguments")
                    region_arg, addr_arg, arr_arg = node.args
                    if not (isinstance(region_arg, ast.Constant) and isinstance(region_arg.value, str)):
                        raise CompileError("writeArray region must be a string literal")

                    mem_name = region_arg.value + "_"
                    if mem_name not in MEM_REGIONS:
                        raise CompileError(f"unknown region '{region_arg.value}'")
                    _, write_imm, _, write_ind, _ = MEM_REGIONS[mem_name]

                    vals = []
                    if isinstance(arr_arg, ast.List):
                        for elt in arr_arg.elts:
                            if not isinstance(elt, ast.Constant): raise CompileError("array must be constants")
                            vals.append(elt.value)
                    elif isinstance(arr_arg, ast.Name):
                        if not hasattr(self, 'array_constants') or arr_arg.id not in self.array_constants:
                            raise CompileError(f"undefined array '{arr_arg.id}'")
                        vals = self.array_constants[arr_arg.id]
                    else:
                        raise CompileError("writeArray requires a list literal or pre-defined array variable")

                    start_addr_op = self.compile_expr(addr_arg, scope)
                    self._emit_write_words(write_imm, write_ind, start_addr_op, vals, scope)
                    return

                elif fname == "writeString":
                    if len(node.args) != 3:
                        raise CompileError("writeString(region, addr, string) takes 3 arguments")
                    region_arg, addr_arg, str_arg = node.args
                    if not (isinstance(region_arg, ast.Constant) and isinstance(region_arg.value, str)):
                        raise CompileError("writeString region must be a string literal")

                    mem_name = region_arg.value + "_"
                    if mem_name not in MEM_REGIONS:
                        raise CompileError(f"unknown region '{region_arg.value}'")
                    _, write_imm, _, write_ind, _ = MEM_REGIONS[mem_name]

                    start_addr_op = self.compile_expr(addr_arg, scope)

                    if isinstance(str_arg, ast.Constant) and isinstance(str_arg.value, str):
                        vals = [ord(c) & 0xFF for c in str_arg.value]
                        self._emit_write_words(write_imm, write_ind, start_addr_op, vals, scope)
                        return

                    if _is_read_string_call(str_arg):
                        # `writeString(region, dst, readString(src_region, src_addr, length))`
                        # copies the runtime string represented by readString's
                        # (source address, fixed length) directly into the destination.
                        self._emit_read_string_to_memory(str_arg, write_ind,
                                                         start_addr_op, scope)
                        return

                    raise CompileError(
                        "writeString string must be a string literal or readString(...)"
                    )

                elif fname == "copyString":
                    # copyString(dst_region, dst_addr, src_region, src_addr, length)
                    # Copies `length` words (character codes) from src to dst.
                    # There is no runtime string type — strings are (addr, length)
                    # pairs — so this is how you "assign" one buffer to another.
                    if len(node.args) != 5:
                        raise CompileError(
                            "copyString(dst_region, dst_addr, src_region, src_addr, length) "
                            "takes 5 arguments"
                        )
                    dst_reg_arg, dst_addr_arg, src_reg_arg, src_addr_arg, len_arg = node.args
                    if not (isinstance(dst_reg_arg, ast.Constant) and isinstance(dst_reg_arg.value, str)):
                        raise CompileError("copyString dst_region must be a string literal")
                    if not (isinstance(src_reg_arg, ast.Constant) and isinstance(src_reg_arg.value, str)):
                        raise CompileError("copyString src_region must be a string literal")
                    dst_name = dst_reg_arg.value + "_"
                    src_name = src_reg_arg.value + "_"
                    if dst_name not in MEM_REGIONS:
                        raise CompileError(f"unknown region '{dst_reg_arg.value}'")
                    if src_name not in MEM_REGIONS:
                        raise CompileError(f"unknown region '{src_reg_arg.value}'")
                    _, write_imm, _, write_ind, _ = MEM_REGIONS[dst_name]
                    _, _, read_imm, _, read_ind = MEM_REGIONS[src_name]
                    if write_imm is None:
                        raise CompileError(f"memory region '{dst_reg_arg.value}' is read-only")

                    dst_op = self.compile_expr(dst_addr_arg, scope)
                    src_op = self.compile_expr(src_addr_arg, scope)
                    len_op = self.compile_expr(len_arg, scope)

                    # Work in temps we can INC freely.
                    if dst_op[0] == "reg" and dst_op[1] in TEMP_REGS:
                        dst_r = dst_op[1]
                    else:
                        dst_r = scope.alloc_temp()
                        if dst_op[0] == "imm":
                            self.emit("LOD", _reg(dst_r), dst_op)
                        else:
                            self.emit_copy_preserving_src(dst_r, dst_op[1])

                    if src_op[0] == "reg" and src_op[1] in TEMP_REGS:
                        src_r = src_op[1]
                    else:
                        src_r = scope.alloc_temp()
                        if src_op[0] == "imm":
                            self.emit("LOD", _reg(src_r), src_op)
                        else:
                            self.emit_copy_preserving_src(src_r, src_op[1])

                    # Loop: for i in 0..len-1 copy one word
                    # Use a countdown so we don't need a separate index register.
                    if len_op[0] == "imm":
                        cnt_r = scope.alloc_temp()
                        self.emit("LOD", _reg(cnt_r), len_op)
                    else:
                        cnt_r = scope.alloc_temp()
                        self.emit_copy_preserving_src(cnt_r, len_op[1])

                    loop_top = self.new_label("copyst_top")
                    loop_end = self.new_label("copyst_end")
                    self.mark(loop_top)
                    self.emit("JZ", _reg(cnt_r), _lbl(loop_end))
                    val_r = scope.alloc_temp()
                    self.emit(read_ind, _reg(val_r), _reg(src_r))
                    self.emit(write_ind, _reg(val_r), _reg(dst_r))
                    scope.free_temp()  # val_r
                    self.emit("INC", _reg(src_r))
                    self.emit("INC", _reg(dst_r))
                    self.emit("DEC", _reg(cnt_r))
                    self.emit("JMP", _lbl(loop_top))
                    self.mark(loop_end)

                    scope.free_temp()  # cnt_r
                    # free src/dst temps if we allocated them (always did via path above)
                    # src_r / dst_r may be the original temps — free by stack discipline
                    if not (src_op[0] == "reg" and src_op[1] in TEMP_REGS):
                        scope.free_temp()
                    if not (dst_op[0] == "reg" and dst_op[1] in TEMP_REGS):
                        scope.free_temp()
                    _free_if_temp(len_op, scope)
                    return

                elif fname == "splitString":
                    # splitString(region, addr, length, out_base, max_args)
                    #
                    # Tokenizes a buffer on ASCII space (32).  Writes into
                    # VOLATILE starting at out_base:
                    #   out_base[0]          = argc  (number of words found)
                    #   out_base[1 + 2*i]    = start offset of word i (relative to addr)
                    #   out_base[2 + 2*i]    = length of word i
                    #
                    # max_args caps how many words are recorded.  Extra words
                    # are ignored.  Empty input yields argc=0.
                    if len(node.args) != 5:
                        raise CompileError(
                            "splitString(region, addr, length, out_base, max_args) "
                            "takes 5 arguments"
                        )
                    region_arg, addr_arg, len_arg, out_arg, max_arg = node.args
                    if not (isinstance(region_arg, ast.Constant) and isinstance(region_arg.value, str)):
                        raise CompileError("splitString region must be a string literal")
                    if not (isinstance(max_arg, ast.Constant) and isinstance(max_arg.value, int)):
                        raise CompileError("splitString max_args must be an integer constant")
                    max_args = max_arg.value
                    if max_args < 1 or max_args > 16:
                        raise CompileError("splitString max_args must be 1..16")

                    mem_name = region_arg.value + "_"
                    if mem_name not in MEM_REGIONS:
                        raise CompileError(f"unknown region '{region_arg.value}'")
                    _, _, read_imm, _, read_ind = MEM_REGIONS[mem_name]

                    addr_op = self.compile_expr(addr_arg, scope)
                    len_op = self.compile_expr(len_arg, scope)
                    out_op = self.compile_expr(out_arg, scope)

                    # Registers we need:
                    #   base_r  – fixed source base address
                    #   i_r     – current index into the source buffer
                    #   len_r   – total length
                    #   out_r   – current write cursor in the out table
                    #   argc_r  – number of words found so far
                    #   ch_r    – current character
                    #   start_r – start offset of the current word
                    #   wlen_r  – length of the current word
                    # That's more than 4 temps, so spill argc/out to memory
                    # slots and keep the hot loop vars in temps.

                    # Spill slots for argc and the out cursor
                    spill_base = self.alloc_save_slots(2)
                    argc_slot = spill_base
                    out_slot = spill_base + 1

                    # base address of the source string
                    if addr_op[0] == "imm":
                        base_r = scope.alloc_temp()
                        self.emit("LOD", _reg(base_r), addr_op)
                    elif addr_op[0] == "reg" and addr_op[1] in TEMP_REGS:
                        base_r = addr_op[1]
                    else:
                        base_r = scope.alloc_temp()
                        self.emit_copy_preserving_src(base_r, addr_op[1])

                    # length
                    if len_op[0] == "imm":
                        len_r = scope.alloc_temp()
                        self.emit("LOD", _reg(len_r), len_op)
                    elif len_op[0] == "reg" and len_op[1] in TEMP_REGS:
                        len_r = len_op[1]
                    else:
                        len_r = scope.alloc_temp()
                        self.emit_copy_preserving_src(len_r, len_op[1])

                    # out cursor starts at out_base + 1 (slot 0 is argc)
                    if out_op[0] == "imm":
                        out_r = scope.alloc_temp()
                        self.emit("LOD", _reg(out_r), _imm((out_op[1] + 1) & 0xFFFF))
                    else:
                        out_r = scope.alloc_temp()
                        if out_op[0] == "reg" and out_op[1] in TEMP_REGS:
                            self.emit_copy_preserving_src(out_r, out_op[1])
                        else:
                            self.emit_copy_preserving_src(out_r, out_op[1])
                        one_r = scope.alloc_temp()
                        self.emit("LOD", _reg(one_r), _imm(1))
                        self.emit("ADD", _reg(out_r), _reg(out_r), _reg(one_r))
                        scope.free_temp()  # one_r

                    # argc = 0
                    zero_r = scope.alloc_temp()
                    self.emit("LOD", _reg(zero_r), _imm(0))
                    self.emit("MOV", _reg(zero_r), _imm(argc_slot))
                    # stash out cursor
                    self.emit("MOV", _reg(out_r), _imm(out_slot))
                    scope.free_temp()  # zero_r
                    scope.free_temp()  # out_r  (reloaded from slot in loop)

                    # i = 0
                    i_r = scope.alloc_temp()
                    self.emit("LOD", _reg(i_r), _imm(0))

                    scan_top = self.new_label("split_scan")
                    scan_end = self.new_label("split_done")
                    self.new_label("split_skipsp")
                    word_start = self.new_label("split_word")
                    word_end = self.new_label("split_wend")
                    after_word = self.new_label("split_after")

                    self.mark(scan_top)
                    # while i < length
                    self.emit("JAE", _reg(i_r), _reg(len_r), _lbl(scan_end))

                    # ch = read(region, base + i)
                    ch_r = scope.alloc_temp()
                    addr_tmp = scope.alloc_temp()
                    self.emit("ADD", _reg(addr_tmp), _reg(base_r), _reg(i_r))
                    self.emit(read_ind, _reg(ch_r), _reg(addr_tmp))
                    scope.free_temp()  # addr_tmp
                    # mask to low byte
                    self.emit_alu3("AND", _reg(ch_r), _reg(ch_r), _imm(0xFF), scope)

                    # if ch == 32: i++; continue (skip leading/inter-word spaces)
                    sp_r = scope.alloc_temp()
                    self.emit("LOD", _reg(sp_r), _imm(32))
                    self.emit("JNE", _reg(ch_r), _reg(sp_r), _lbl(word_start))
                    scope.free_temp()  # sp_r
                    scope.free_temp()  # ch_r
                    self.emit("INC", _reg(i_r))
                    self.emit("JMP", _lbl(scan_top))

                    # --- start of a word ---
                    self.mark(word_start)
                    # (ch_r and sp_r still live from above only on the JNE path;
                    #  on the JNE-taken path we did NOT free them. Free sp, keep ch.)
                    # Actually on JNE path sp_r and ch_r are still allocated.
                    # Free sp_r; ch_r still needed? not really past here.
                    scope.free_temp()  # sp_r
                    scope.free_temp()  # ch_r

                    # if argc >= max_args: stop
                    argc_r = scope.alloc_temp()
                    self.emit("LDM", _reg(argc_r), _imm(argc_slot))
                    max_r = scope.alloc_temp()
                    self.emit("LOD", _reg(max_r), _imm(max_args))
                    self.emit("JAE", _reg(argc_r), _reg(max_r), _lbl(scan_end))
                    scope.free_temp()  # max_r

                    # start = i
                    start_r = scope.alloc_temp()
                    self.emit_copy_preserving_src(start_r, i_r)

                    # advance i until space or end
                    self.mark(word_end)
                    self.emit("JAE", _reg(i_r), _reg(len_r), _lbl(after_word))
                    ch_r = scope.alloc_temp()
                    addr_tmp = scope.alloc_temp()
                    self.emit("ADD", _reg(addr_tmp), _reg(base_r), _reg(i_r))
                    self.emit(read_ind, _reg(ch_r), _reg(addr_tmp))
                    scope.free_temp()  # addr_tmp
                    self.emit_alu3("AND", _reg(ch_r), _reg(ch_r), _imm(0xFF), scope)
                    sp_r = scope.alloc_temp()
                    self.emit("LOD", _reg(sp_r), _imm(32))
                    # Free temps before the branch so both paths stay balanced.
                    # JE taken → word ends on space; fall-through → keep scanning.
                    self.emit("JE", _reg(ch_r), _reg(sp_r), _lbl(after_word_free))
                    scope.free_temp()  # sp_r
                    scope.free_temp()  # ch_r
                    self.emit("INC", _reg(i_r))
                    self.emit("JMP", _lbl(word_end))

                    self.mark(after_word_free)
                    scope.free_temp()  # sp_r
                    scope.free_temp()  # ch_r

                    self.mark(after_word)
                    # wlen = i - start
                    wlen_r = scope.alloc_temp()
                    self.emit("SUB", _reg(wlen_r), _reg(i_r), _reg(start_r))

                    # write start and wlen into out table
                    out_r = scope.alloc_temp()
                    self.emit("LDM", _reg(out_r), _imm(out_slot))
                    self.emit("MOV", _reg(start_r), _reg(out_r))   # out[cursor] = start
                    self.emit("INC", _reg(out_r))
                    self.emit("MOV", _reg(wlen_r), _reg(out_r))    # out[cursor+1] = wlen
                    self.emit("INC", _reg(out_r))
                    self.emit("MOV", _reg(out_r), _imm(out_slot))
                    scope.free_temp()  # out_r
                    scope.free_temp()  # wlen_r
                    scope.free_temp()  # start_r

                    # argc++
                    self.emit("INC", _reg(argc_r))
                    self.emit("MOV", _reg(argc_r), _imm(argc_slot))
                    scope.free_temp()  # argc_r

                    self.emit("JMP", _lbl(scan_top))

                    self.mark(scan_end)
                    # Write final argc into out_base[0]
                    argc_r = scope.alloc_temp()
                    self.emit("LDM", _reg(argc_r), _imm(argc_slot))
                    if out_op[0] == "imm":
                        self.emit("MOV", _reg(argc_r), out_op)
                    else:
                        # need address in a reg
                        if out_op[0] == "reg" and out_op[1] in TEMP_REGS:
                            self.emit("MOVI", _reg(argc_r), out_op)
                        else:
                            out_addr_r = scope.alloc_temp()
                            self.emit_copy_preserving_src(out_addr_r, out_op[1])
                            self.emit("MOVI", _reg(argc_r), _reg(out_addr_r))
                            scope.free_temp()
                    scope.free_temp()  # argc_r

                    # Free base_r / len_r / i_r
                    scope.free_temp()  # i_r
                    # base_r and len_r — free if we allocated (always treated as temps)
                    # Use conservative free: both were alloc_temp'd in the paths above
                    # except when the original expr was already a temp.
                    if not (len_op[0] == "reg" and len_op[1] in TEMP_REGS):
                        scope.free_temp()
                    if not (addr_op[0] == "reg" and addr_op[1] in TEMP_REGS):
                        scope.free_temp()
                    return

                elif fname in ("readArray", "readString"):
                    if len(node.args) != 3:
                        raise CompileError(f"{fname}(region, addr, length) takes 3 arguments")
                    region_arg, addr_arg, len_arg = node.args
                    if not (isinstance(region_arg, ast.Constant) and isinstance(region_arg.value, str)):
                        raise CompileError(f"{fname} region must be a string literal")
                    if not (isinstance(len_arg, ast.Constant) and isinstance(len_arg.value, int)):
                        raise CompileError(f"{fname} length must be an integer constant")

                    mem_name = region_arg.value + "_"
                    if mem_name not in MEM_REGIONS:
                        raise CompileError(f"unknown region '{region_arg.value}'")
                    _, _, read_imm, _, read_ind = MEM_REGIONS[mem_name]
                    length = len_arg.value

                    # There's no array/string runtime value in this register
                    # machine, so - same idea as `print(read(region, addr))` -
                    # each word is read and printed in turn. readString masks
                    # to the low byte (the character code) first.
                    addr_op = self.compile_expr(addr_arg, scope)
                    if addr_op[0] == "reg" and addr_op[1] in TEMP_REGS:
                        addr_reg = addr_op[1]          # already a scratch temp, safe to mutate
                    else:
                        addr_reg = scope.alloc_temp()  # copy so we never INC the caller's variable
                        if addr_op[0] == "imm":
                            self.emit("LOD", _reg(addr_reg), addr_op)
                        else:
                            self.emit_copy_preserving_src(addr_reg, addr_op[1])

                    val_reg = scope.alloc_temp()
                    for i in range(length):
                        self.emit(read_ind, _reg(val_reg), _reg(addr_reg))
                        if fname == "readString":
                            self.emit_alu3("AND", _reg(val_reg), _reg(val_reg), _imm(0xFF), scope)
                        self.emit("PRT", _reg(val_reg))
                        if i < length - 1:
                            self.emit("INC", _reg(addr_reg))
                    scope.free_temp()  # val_reg
                    scope.free_temp()  # addr_reg
                    return

                if fname == "cls":
                    color_arg = node.args[0] if node.args else ast.Constant(value=0)
                    src = self.compile_expr(color_arg, scope)
                    if src[0] == "imm":
                        src = self._imm_to_reg(src, scope)
                    self.emit("VMV", src, _imm(0xFFFF))
                    _free_if_temp(src, scope)
                    return
                elif fname == "frameBlit":
                    self.emit("FLP")
                    return
                elif fname == "wait":
                    # wait(frames): issue exactly `frames` FLPs.
                    #
                    # For more stable wall-clock timing under load we key the
                    # exit condition off the hardware frame counter (FRC_): the
                    # loop continues until (FRC_ - start) >= frames (unsigned).
                    # Each iteration still emits FLP so the host advances the
                    # display/audio. FRC_ is snapshotted once before the loop.
                    if len(node.args) != 1:
                        raise CompileError("wait(frames) takes exactly 1 argument")
                    n_op = self.compile_expr(node.args[0], scope)
                    if n_op[0] == "imm" and n_op[1] == 0:
                        return

                    # Temps: start, n (if imm), cur, elapsed  — at most 4
                    start_reg = scope.alloc_temp()
                    self.emit("LDM", _reg(start_reg), _imm(0xFD))  # FRC_ snapshot

                    n_reg = self._to_reg(n_op, scope)
                    n_was_temp = (n_reg[0] == "reg" and n_reg[1] in TEMP_REGS)

                    cur_reg = scope.alloc_temp()
                    elapsed_reg = scope.alloc_temp()
                    loop_lbl = self.new_label("wait")
                    self.mark(loop_lbl)
                    self.emit("FLP")
                    self.emit("LDM", _reg(cur_reg), _imm(0xFD))
                    # elapsed = (cur - start) mod 65536  (unsigned)
                    self.emit("SUB", _reg(elapsed_reg), _reg(cur_reg), _reg(start_reg))
                    # keep going while elapsed < n
                    self.emit("JB", _reg(elapsed_reg), n_reg, _lbl(loop_lbl))

                    scope.free_temp()  # elapsed
                    scope.free_temp()  # cur
                    if n_was_temp:
                        scope.free_temp()  # n
                    scope.free_temp()  # start
                    return
                elif fname == "shutdown":
                    self.emit("BRK")
                    return
                elif fname == "execProgram":
                    # execProgram(name): one-way chain-load. Loads another
                    # compiled program (a disk entry, packed by mkdisk.py)
                    # straight into NPROG and jumps to it - this program's
                    # own code is gone the moment it happens, same as a
                    # console swapping carts. There's no return; if the
                    # launched program needs to hand control back, use
                    # execProgram() again by name. VOLATILE (and so registers'
                    # saved values, if you stash any there yourself) survives
                    # the jump, so it's the way to pass data across.
                    if len(node.args) != 1:
                        raise CompileError("execProgram(name) takes exactly 1 argument")
                    name_arg = node.args[0]
                    self._emit_disk_name_arg(name_arg, scope)
                    self._emit_disk_trigger(DISK_CMD_EXEC, scope)
                    return
                elif fname == "diskSave":
                    # diskSave(name, region, addr, length): writes `length`
                    # words from region:addr to disk under `name`, creating
                    # or overwriting that entry. region is "VOLATILE",
                    # "DISK", or "VRAM".
                    if len(node.args) != 4:
                        raise CompileError("diskSave(name, region, addr, length) takes exactly 4 arguments")
                    name_arg, region_arg, addr_arg, len_arg = node.args
                    if not (isinstance(region_arg, ast.Constant) and isinstance(region_arg.value, str)):
                        raise CompileError("diskSave region must be a string literal")
                    self._emit_disk_name_arg(name_arg, scope)
                    addr_op = self.compile_expr(addr_arg, scope)
                    addr_reg = self._to_reg(addr_op, scope)
                    self.emit("MOV", addr_reg, _imm(DISK_ADDR))
                    _free_if_temp(addr_reg, scope)
                    len_op = self.compile_expr(len_arg, scope)
                    len_reg = self._to_reg(len_op, scope)
                    self.emit("MOV", len_reg, _imm(DISK_LEN))
                    _free_if_temp(len_reg, scope)
                    self._emit_disk_region(region_arg.value, scope)
                    self._emit_disk_trigger(DISK_CMD_SAVE, scope)
                    return
                elif fname == "diskDelete":
                    if len(node.args) != 1:
                        raise CompileError("diskDelete(name) takes exactly 1 argument")
                    name_arg = node.args[0]
                    self._emit_disk_name_arg(name_arg, scope)
                    self._emit_disk_trigger(DISK_CMD_DELETE, scope)
                    return
                elif fname == "setPixel":
                    if len(node.args) != 3:
                        raise CompileError("setPixel(x, y, color) takes exactly 3 arguments")
                    x_op, y_op, c_op = [self.compile_expr(a, scope) for a in node.args]
                    self.emit_gpu_command(GPU_CMD_SETPIXEL, [
                        (GPU_X0, x_op), (GPU_Y0, y_op), (GPU_COLOR, c_op),
                    ], scope)
                    return
                elif fname == "drawLine":
                    if len(node.args) != 5:
                        raise CompileError("drawLine(x0, y0, x1, y1, color) takes exactly 5 arguments")
                    x0_op, y0_op, x1_op, y1_op, c_op = [self.compile_expr(a, scope) for a in node.args]
                    self.emit_gpu_command(GPU_CMD_DRAWLINE, [
                        (GPU_X0, x0_op), (GPU_Y0, y0_op),
                        (GPU_X1, x1_op), (GPU_Y1, y1_op),
                        (GPU_COLOR, c_op),
                    ], scope)
                    return

                # --- November Chip helpers (kept in sync with nupy.py) ---
                elif fname == "setMasterColor":
                    if len(node.args) != 4:
                        raise CompileError("setMasterColor(index, r, g, b) takes 4 arguments")
                    if all(isinstance(a, ast.Constant) and isinstance(a.value, int) for a in node.args):
                        index, r, g, b = [a.value for a in node.args]
                        word = (((r >> 4) & 15) << 8) | (((g >> 4) & 15) << 4) | ((b >> 4) & 15)
                        addr = MASTER_PALETTE_BASE + (index & 4095)
                        t = scope.alloc_temp()
                        self.emit("LOD", _reg(t), _imm(word))
                        self.emit("VMV", _reg(t), _imm(addr))
                        scope.free_temp()
                        return
                    word = scope.alloc_temp()
                    tmp = scope.alloc_temp()
                    def _pack_nibble(src_node, shift_amt):
                        opr = self.compile_expr(src_node, scope)
                        if opr[0] == "imm":
                            v = ((opr[1] >> 4) & 15) << shift_amt
                            self.emit("LOD", _reg(tmp), _imm(v))
                        else:
                            self.emit("SHR", _reg(tmp), opr, _imm(4))
                            self.emit_alu3("AND", _reg(tmp), _reg(tmp), _imm(15), scope)
                            if shift_amt:
                                self.emit("SHL", _reg(tmp), _reg(tmp), _imm(shift_amt))
                            _free_if_temp(opr, scope)
                    _pack_nibble(node.args[1], 8)
                    self.emit("SWP", _reg(word), _reg(tmp))
                    _pack_nibble(node.args[2], 4)
                    self.emit("OR", _reg(word), _reg(word), _reg(tmp))
                    _pack_nibble(node.args[3], 0)
                    self.emit("OR", _reg(word), _reg(word), _reg(tmp))
                    idx_op = self.compile_expr(node.args[0], scope)
                    if idx_op[0] == "imm":
                        addr = MASTER_PALETTE_BASE + (idx_op[1] & 4095)
                        self.emit("VMV", _reg(word), _imm(addr))
                    else:
                        self.emit_alu3("AND", _reg(tmp), idx_op, _imm(4095), scope)
                        self.emit_alu3("ADD", _reg(tmp), _reg(tmp), _imm(MASTER_PALETTE_BASE), scope)
                        self.emit("VMV", _reg(word), _reg(tmp))
                        _free_if_temp(idx_op, scope)
                    scope.free_temp(); scope.free_temp()
                    return

                elif fname == "setActivePalette":
                    if len(node.args) != 2:
                        raise CompileError("setActivePalette(slot, master_index) takes 2 arguments")
                    if all(isinstance(a, ast.Constant) and isinstance(a.value, int) for a in node.args):
                        slot, mi = [a.value for a in node.args]
                        addr = ACTIVE_PALETTE_BASE + (slot & 63)
                        t = scope.alloc_temp()
                        self.emit("LOD", _reg(t), _imm(mi & 4095))
                        self.emit("VMV", _reg(t), _imm(addr))
                        scope.free_temp()
                        return
                    slot_op = self.compile_expr(node.args[0], scope)
                    slot_reg = self._to_reg(slot_op, scope)
                    t1 = scope.alloc_temp()
                    self.emit_alu3("AND", _reg(t1), slot_reg, _imm(63), scope)
                    self.emit_alu3("ADD", _reg(t1), _reg(t1), _imm(ACTIVE_PALETTE_BASE), scope)
                    _free_if_temp(slot_reg, scope)
                    mi_op = self.compile_expr(node.args[1], scope)
                    mi_reg = self._to_reg(mi_op, scope)
                    t2 = scope.alloc_temp()
                    self.emit_alu3("AND", _reg(t2), mi_reg, _imm(4095), scope)
                    self.emit("VMV", _reg(t2), _reg(t1))
                    scope.free_temp(); scope.free_temp()
                    _free_if_temp(mi_reg, scope)
                    return

                elif fname == "writeBitplaneWord":
                    if len(node.args) != 3:
                        raise CompileError("writeBitplaneWord(plane, word_idx, val) takes 3 arguments")
                    if all(isinstance(a, ast.Constant) and isinstance(a.value, int) for a in node.args):
                        plane, widx, val = [a.value for a in node.args]
                        addr = BITPLANE_BASE + plane * PLANE_SIZE + widx
                        t = scope.alloc_temp()
                        self.emit("LOD", _reg(t), _imm(val))
                        self.emit("VMV", _reg(t), _imm(addr))
                        scope.free_temp()
                        return
                    plane_op = self.compile_expr(node.args[0], scope)
                    plane_reg = self._to_reg(plane_op, scope)
                    t1 = scope.alloc_temp()
                    self.emit_alu3("MUL", _reg(t1), plane_reg, _imm(PLANE_SIZE), scope)
                    _free_if_temp(plane_reg, scope)
                    widx_op = self.compile_expr(node.args[1], scope)
                    widx_reg = self._to_reg(widx_op, scope)
                    self.emit("ADD", _reg(t1), _reg(t1), widx_reg)
                    _free_if_temp(widx_reg, scope)
                    val_op = self.compile_expr(node.args[2], scope)
                    val_reg = self._to_reg(val_op, scope)
                    self.emit("VMV", val_reg, _reg(t1))
                    scope.free_temp()
                    _free_if_temp(val_reg, scope)
                    return

                elif fname == "setSpritePos":
                    if len(node.args) != 3:
                        raise CompileError("setSpritePos(sprite_id, x, y) takes 3 arguments")
                    if all(isinstance(a, ast.Constant) and isinstance(a.value, int) for a in node.args):
                        sid, x, y = [a.value for a in node.args]
                        base = SPRITE_ATTRS_BASE + sid * 8
                        t = scope.alloc_temp()
                        self.emit("LOD", _reg(t), _imm(x))
                        self.emit("VMV", _reg(t), _imm(base))
                        self.emit("LOD", _reg(t), _imm(y))
                        self.emit("VMV", _reg(t), _imm(base + 1))
                        scope.free_temp()
                        return
                    sid_op = self.compile_expr(node.args[0], scope)
                    sid_reg = self._to_reg(sid_op, scope)
                    t1 = scope.alloc_temp()
                    self.emit_alu3("MUL", _reg(t1), sid_reg, _imm(8), scope)
                    self.emit_alu3("ADD", _reg(t1), _reg(t1), _imm(SPRITE_ATTRS_BASE), scope)
                    _free_if_temp(sid_reg, scope)
                    x_op = self.compile_expr(node.args[1], scope)
                    x_reg = self._to_reg(x_op, scope)
                    self.emit("VMV", x_reg, _reg(t1))
                    _free_if_temp(x_reg, scope)
                    self.emit("INC", _reg(t1))
                    y_op = self.compile_expr(node.args[2], scope)
                    y_reg = self._to_reg(y_op, scope)
                    self.emit("VMV", y_reg, _reg(t1))
                    scope.free_temp()
                    _free_if_temp(y_reg, scope)
                    return

                elif fname == "setSpriteAttr":
                    if len(node.args) != 6:
                        raise CompileError("setSpriteAttr(sprite_id, flags, scale_x, scale_y, rot, skew) takes 6 arguments")
                    if all(isinstance(a, ast.Constant) and isinstance(a.value, int) for a in node.args):
                        sid, flags, sx, sy, rot, skew = [a.value for a in node.args]
                        base = SPRITE_ATTRS_BASE + sid * 8 + 2
                        t = scope.alloc_temp()
                        for i, v in enumerate([flags, sx, sy, rot, skew]):
                            self.emit("LOD", _reg(t), _imm(v))
                            self.emit("VMV", _reg(t), _imm(base + i))
                        scope.free_temp()
                        return
                    sid_op = self.compile_expr(node.args[0], scope)
                    sid_reg = self._to_reg(sid_op, scope)
                    t1 = scope.alloc_temp()
                    self.emit_alu3("MUL", _reg(t1), sid_reg, _imm(8), scope)
                    self.emit_alu3("ADD", _reg(t1), _reg(t1), _imm(SPRITE_ATTRS_BASE + 2), scope)
                    _free_if_temp(sid_reg, scope)
                    for i, arg in enumerate(node.args[1:]):
                        op = self.compile_expr(arg, scope)
                        reg = self._to_reg(op, scope)
                        self.emit("VMV", reg, _reg(t1))
                        _free_if_temp(reg, scope)
                        if i < 4:
                            self.emit("INC", _reg(t1))
                    scope.free_temp()
                    return

                elif fname == "setSpriteColor":
                    if len(node.args) != 3:
                        raise CompileError("setSpriteColor(sprite_id, palette_index, master_index) takes 3 arguments")
                    if all(isinstance(a, ast.Constant) and isinstance(a.value, int) for a in node.args):
                        sid, pi, mi = [a.value for a in node.args]
                        addr = SPRITE_PAL_BASE + sid * 16 + (pi & 15)
                        t = scope.alloc_temp()
                        self.emit("LOD", _reg(t), _imm(mi & 4095))
                        self.emit("VMV", _reg(t), _imm(addr))
                        scope.free_temp()
                        return
                    sid_op = self.compile_expr(node.args[0], scope)
                    sid_reg = self._to_reg(sid_op, scope)
                    t1 = scope.alloc_temp()
                    self.emit_alu3("MUL", _reg(t1), sid_reg, _imm(16), scope)
                    _free_if_temp(sid_reg, scope)
                    pi_op = self.compile_expr(node.args[1], scope)
                    pi_reg = self._to_reg(pi_op, scope)
                    t2 = scope.alloc_temp()
                    self.emit_alu3("AND", _reg(t2), pi_reg, _imm(15), scope)
                    self.emit("ADD", _reg(t1), _reg(t1), _reg(t2))
                    self.emit_alu3("ADD", _reg(t1), _reg(t1), _imm(SPRITE_PAL_BASE), scope)
                    _free_if_temp(pi_reg, scope)
                    mi_op = self.compile_expr(node.args[2], scope)
                    mi_reg = self._to_reg(mi_op, scope)
                    self.emit_alu3("AND", _reg(t2), mi_reg, _imm(4095), scope)
                    self.emit("VMV", _reg(t2), _reg(t1))
                    scope.free_temp(); scope.free_temp()
                    _free_if_temp(mi_reg, scope)
                    return

                elif fname == "setSplitScreen":
                    if len(node.args) != 3:
                        raise CompileError("setSplitScreen(enabled, axis, pos) takes 3 arguments")
                    en_op = self.compile_expr(node.args[0], scope)
                    en_reg = self._to_reg(en_op, scope)
                    self.emit("VMV", en_reg, _imm(SPLIT_ENABLE))
                    _free_if_temp(en_reg, scope)
                    ax_op = self.compile_expr(node.args[1], scope)
                    ax_reg = self._to_reg(ax_op, scope)
                    self.emit("VMV", ax_reg, _imm(SPLIT_AXIS))
                    _free_if_temp(ax_reg, scope)
                    pos_op = self.compile_expr(node.args[2], scope)
                    pos_reg = self._to_reg(pos_op, scope)
                    self.emit("VMV", pos_reg, _imm(SPLIT_POS))
                    _free_if_temp(pos_reg, scope)
                    return

                elif fname == "setHScroll":
                    if len(node.args) != 2:
                        raise CompileError("setHScroll(zone_a, zone_b) takes 2 arguments")
                    a_op = self.compile_expr(node.args[0], scope)
                    a_reg = self._to_reg(a_op, scope)
                    self.emit("VMV", a_reg, _imm(HSCROLL_A))
                    _free_if_temp(a_reg, scope)
                    b_op = self.compile_expr(node.args[1], scope)
                    b_reg = self._to_reg(b_op, scope)
                    self.emit("VMV", b_reg, _imm(HSCROLL_B))
                    _free_if_temp(b_reg, scope)
                    return

                # --- Interrupt / stack intrinsics ---
                elif fname == "enableInterrupts":
                    self.emit("SEI")
                    return
                elif fname == "disableInterrupts":
                    self.emit("CLI")
                    return
                elif fname == "returnFromInterrupt":
                    self.emit("RTI")
                    return
                elif fname == "softwareInterrupt":
                    self.emit("SWI")
                    return
                elif fname == "push":
                    if len(node.args) != 1:
                        raise CompileError("push(expr) takes exactly 1 argument")
                    src = self.compile_expr(node.args[0], scope)
                    if src[0] == "imm":
                        src = self._imm_to_reg(src, scope)
                    self.emit("PSH", src)
                    _free_if_temp(src, scope)
                    return
                elif fname == "pop":
                    if len(node.args) != 1 or not isinstance(node.args[0], ast.Name):
                        raise CompileError("pop(var) requires a variable name")
                    dest = scope.var_reg(node.args[0].id)
                    self.emit("POP", _reg(dest))
                    return
                elif fname == "setStackPointer":
                    if len(node.args) != 1:
                        raise CompileError("setStackPointer(value) takes 1 argument")
                    src = self.compile_expr(node.args[0], scope)
                    if src[0] == "imm":
                        src = self._imm_to_reg(src, scope)
                    self.emit("LSP", src)
                    _free_if_temp(src, scope)
                    return
                elif fname == "setIrqVector":
                    if len(node.args) != 2:
                        raise CompileError("setIrqVector(source, addr) takes 2 arguments")
                    src_arg, addr_arg = node.args
                    if not (isinstance(src_arg, ast.Constant) and isinstance(src_arg.value, int)):
                        raise CompileError("setIrqVector source must be an integer constant")
                    source = src_arg.value
                    if source < 0 or source > 7:
                        raise CompileError("IRQ source must be 0..7")
                    if isinstance(addr_arg, ast.Name) and addr_arg.id in self.funcs:
                        addr_op = _lbl(self.funcs[addr_arg.id].start_label)
                        val_reg = scope.alloc_temp()
                        self.emit("LOD", _reg(val_reg), addr_op)
                        self.emit("MOV", _reg(val_reg), _imm(IRQ_VECTOR_BASE + source))
                        scope.free_temp()
                    else:
                        addr_op = self.compile_expr(addr_arg, scope)
                        if addr_op[0] == "imm":
                            val_reg = scope.alloc_temp()
                            self.emit("LOD", _reg(val_reg), addr_op)
                            self.emit("MOV", _reg(val_reg), _imm(IRQ_VECTOR_BASE + source))
                            scope.free_temp()
                        else:
                            self.emit("MOV", addr_op, _imm(IRQ_VECTOR_BASE + source))
                            _free_if_temp(addr_op, scope)
                    return
                elif fname == "setIrqMask":
                    if len(node.args) != 1:
                        raise CompileError("setIrqMask(mask) takes 1 argument")
                    src = self.compile_expr(node.args[0], scope)
                    if src[0] == "imm":
                        src = self._imm_to_reg(src, scope)
                    self.emit("MOV", src, _imm(IRQ_MASK))
                    _free_if_temp(src, scope)
                    return
                elif fname == "setTimer":
                    if len(node.args) != 1:
                        raise CompileError("setTimer(reload) takes 1 argument")
                    src = self.compile_expr(node.args[0], scope)
                    if src[0] == "imm":
                        src = self._imm_to_reg(src, scope)
                    self.emit("MOV", src, _imm(IRQ_TIMER_RELOAD))
                    _free_if_temp(src, scope)
                    return
                elif fname == "clearIrq":
                    if len(node.args) != 1:
                        raise CompileError("clearIrq(bits) takes 1 argument")
                    src = self.compile_expr(node.args[0], scope)
                    if src[0] == "imm":
                        src = self._imm_to_reg(src, scope)
                    self.emit("MOV", src, _imm(IRQ_PENDING))
                    _free_if_temp(src, scope)
                    return

                # --- Afterburner II Audio Intrinsics ---
                elif fname == "setPSGVolume":
                    if len(node.args) != 3:
                        raise CompileError("setPSGVolume(master, psg1, psg2) takes 3 arguments")

                    # 1. Process Master Volume (Shift in place)
                    m_op = self.compile_expr(node.args[0], scope)
                    t1 = self._to_reg(m_op, scope)
                    self.emit("SHL", t1, t1, _imm(8))

                    # 2. Process PSG1 Volume (Shift in place, then OR into t1)
                    p1_op = self.compile_expr(node.args[1], scope)
                    reg_p1 = self._to_reg(p1_op, scope)
                    self.emit("SHL", reg_p1, reg_p1, _imm(4))
                    self.emit("OR", t1, t1, reg_p1)
                    _free_if_temp(reg_p1, scope)

                    # 3. Process PSG2 Volume (OR directly into t1)
                    p2_op = self.compile_expr(node.args[2], scope)
                    reg_p2 = self._to_reg(p2_op, scope)
                    self.emit("OR", t1, t1, reg_p2)
                    _free_if_temp(reg_p2, scope)

                    # 4. Write composite value directly to 0x9000
                    self.emit("MOV", t1, _imm(0x9000))
                    _free_if_temp(t1, scope)
                    return

                elif fname == "setPSG3Control":
                    if len(node.args) != 2:
                        raise CompileError("setPSG3Control(volume, wave_enable) takes 2 arguments")

                    v_op = self.compile_expr(node.args[0], scope)
                    t1 = self._to_reg(v_op, scope)
                    self.emit("SHL", t1, t1, _imm(8))

                    we_op = self.compile_expr(node.args[1], scope)
                    we_reg = self._to_reg(we_op, scope)
                    self.emit("SHL", we_reg, we_reg, _imm(4))
                    self.emit("OR", t1, t1, we_reg)
                    _free_if_temp(we_reg, scope)

                    self.emit("MOV", t1, _imm(0x9001))
                    _free_if_temp(t1, scope)
                    return

                elif fname == "setPSGWaveform":
                    # setPSGWaveform(w0, w1, w2 [, and_enable])
                    # Packs 2-bit waveforms into bits 13-12 / 9-8 / 5-4.
                    # Optional 4th arg: non-zero sets bit 15 (PSG2 AND-with-PSG3).
                    if len(node.args) not in (3, 4):
                        raise CompileError("setPSGWaveform(w0, w1, w2 [, and_enable]) takes 3 or 4 arguments")

                    t1 = scope.alloc_temp()
                    self.emit("LOD", _reg(t1), _imm(0))

                    for i, arg in enumerate(node.args[:3]):
                        op = self.compile_expr(arg, scope)
                        reg = self._to_reg(op, scope)
                        # Mask to 2 bits then shift into place (12, 8, 4)
                        self.emit_alu3("AND", reg, reg, _imm(0x3), scope)
                        shift = 12 - (i * 4)
                        if shift > 0:
                            self.emit("SHL", reg, reg, _imm(shift))
                        self.emit("OR", _reg(t1), _reg(t1), reg)
                        _free_if_temp(reg, scope)

                    if len(node.args) == 4:
                        # and_enable -> bit 15
                        and_op = self.compile_expr(node.args[3], scope)
                        and_reg = self._to_reg(and_op, scope)
                        # if and_enable != 0: set bit 15
                        # Use a simple non-zero test via OR of shifted flag
                        # (non-zero value becomes 0x8000)
                        zero_lbl = self.new_label("psg_and_skip")
                        self.emit("JZ", and_reg, _lbl(zero_lbl))
                        or_reg = scope.alloc_temp()
                        self.emit("LOD", _reg(or_reg), _imm(0x8000))
                        self.emit("OR", _reg(t1), _reg(t1), _reg(or_reg))
                        scope.free_temp()
                        self.mark(zero_lbl)
                        _free_if_temp(and_reg, scope)

                    self.emit("MOV", _reg(t1), _imm(0x9002))
                    scope.free_temp()
                    return

                elif fname == "setPSGDuty":
                    # setPSGDuty(d0, d1, d2) - 4-bit fields in bits 15-12 / 11-8 / 7-4
                    if len(node.args) != 3:
                        raise CompileError("setPSGDuty(d0, d1, d2) takes 3 arguments")

                    t1 = scope.alloc_temp()
                    self.emit("LOD", _reg(t1), _imm(0))

                    for i, arg in enumerate(node.args):
                        op = self.compile_expr(arg, scope)
                        reg = self._to_reg(op, scope)
                        self.emit_alu3("AND", reg, reg, _imm(0xF), scope)
                        shift = 12 - (i * 4)
                        if shift > 0:
                            self.emit("SHL", reg, reg, _imm(shift))
                        self.emit("OR", _reg(t1), _reg(t1), reg)
                        _free_if_temp(reg, scope)

                    self.emit("MOV", _reg(t1), _imm(0x9003))
                    scope.free_temp()
                    return

                elif fname == "setPSGTone":
                    if len(node.args) != 2:
                        raise CompileError("setPSGTone(channel, period) takes 2 arguments")
                    ch_arg, per_arg = node.args
                    if isinstance(ch_arg, ast.Constant) and isinstance(ch_arg.value, int):
                        ch = ch_arg.value
                        addr = 0x9004 + ch
                        per_op = self.compile_expr(per_arg, scope)
                        per_reg = self._to_reg(per_op, scope)
                        self.emit("MOV", per_reg, _imm(addr))
                        _free_if_temp(per_reg, scope)
                        return
                    ch_op = self.compile_expr(ch_arg, scope)
                    ch_reg = self._to_reg(ch_op, scope)
                    t1 = scope.alloc_temp()
                    self.emit_alu3("ADD", _reg(t1), ch_reg, _imm(0x9004), scope)
                    per_op = self.compile_expr(per_arg, scope)
                    per_reg = self._to_reg(per_op, scope)
                    self.emit("MOVI", per_reg, _reg(t1))
                    scope.free_temp()
                    _free_if_temp(ch_reg, scope)
                    _free_if_temp(per_reg, scope)
                    return

                elif fname == "setPSG3Wavetable":
                    if len(node.args) != 2:
                        raise CompileError("setPSG3Wavetable(index, word_val) takes 2 arguments")
                    idx_op = self.compile_expr(node.args[0], scope)
                    val_op = self.compile_expr(node.args[1], scope)
                    idx_reg = self._to_reg(idx_op, scope)
                    val_reg = self._to_reg(val_op, scope)
                    t1 = scope.alloc_temp()
                    self.emit_alu3("AND", _reg(t1), idx_reg, _imm(7), scope)
                    self.emit_alu3("ADD", _reg(t1), _reg(t1), _imm(0x9010), scope)
                    self.emit("MOVI", val_reg, _reg(t1))
                    scope.free_temp()
                    _free_if_temp(idx_reg, scope)
                    _free_if_temp(val_reg, scope)
                    return

        name = node.func.id if isinstance(node.func, ast.Name) else "?"
        if name == "print":
            if len(node.args) != 1:
                raise CompileError("print() takes exactly one argument")
            src = self.compile_expr(node.args[0], scope)
            if src[0] == "imm":
                src = self._imm_to_reg(src, scope)
            self.emit("PRT", src)
            if src[1] in TEMP_REGS:
                scope.free_temp()
        else:
            self.compile_call(node, scope, dest_reg=None)

    def compile_if(self, node, scope, break_label, continue_label):
        else_label = self.new_label("else")
        end_label = self.new_label("endif")
        self.compile_branch_on_false(node.test, scope, else_label)
        for s in node.body:
            self.compile_stmt(s, scope, break_label, continue_label)
        if node.orelse:
            self.emit("JMP", _lbl(end_label))
        self.mark(else_label)
        for s in node.orelse:
            self.compile_stmt(s, scope, break_label, continue_label)
        self.mark(end_label)

    def compile_while(self, node, scope):
        top = self.new_label("while")
        end = self.new_label("endwhile")
        self.mark(top)
        self.compile_branch_on_false(node.test, scope, end)
        for s in node.body:
            self.compile_stmt(s, scope, end, top)
        self.emit("JMP", _lbl(top))
        self.mark(end)

    def compile_for(self, node, scope):
        if not (isinstance(node.target, ast.Name)
                and isinstance(node.iter, ast.Call)
                and isinstance(node.iter.func, ast.Name)
                and node.iter.func.id == "range"):
            raise CompileError("only 'for x in range(...)' loops are supported")

        args = node.iter.args
        if len(args) == 1:
            start_node, stop_node = None, args[0]
        elif len(args) == 2:
            start_node, stop_node = args[0], args[1]
        else:
            raise CompileError("range() with a step is not supported (step must be 1)")

        if not isinstance(stop_node, ast.Constant):
            raise CompileError("range() bound must be a constant for now")
        stop = stop_node.value
        start = start_node.value if start_node is not None else 0

        counter = scope.var_reg(node.target.id)
        self.emit("LOD", _reg(counter), _imm(start))

        body = self.new_label("for")
        end = self.new_label("endfor")
        cont = self.new_label("forcont")
        self.mark(body)
        for s in node.body:
            self.compile_stmt(s, scope, end, cont)
        self.mark(cont)
        self.emit("FOR", _reg(counter), _imm(stop), _lbl(body))
        self.mark(end)

    def _compile_string_literal_branch(self, call, literal, is_eq, scope, target, want_true_jump):
        """Compile `readString(region, addr, length) == "literal"` (or !=)
        as a real runtime comparison: check the length first, then compare
        each character. There's no runtime string type in this machine, so
        this is only supported directly as an if/while condition, same as
        every other comparison in this language."""
        if len(call.args) != 3:
            raise CompileError("readString(region, addr, length) takes exactly 3 arguments")
        region_arg, addr_arg, len_arg = call.args
        if not (isinstance(region_arg, ast.Constant) and isinstance(region_arg.value, str)):
            raise CompileError("readString region must be a string literal")
        mem_name = region_arg.value + "_"
        if mem_name not in MEM_REGIONS:
            raise CompileError(f"unknown region '{region_arg.value}'")
        _, _, read_imm, _, read_ind = MEM_REGIONS[mem_name]

        # Should we branch once we've *confirmed a match*, or as soon as we
        # find any mismatch? Eq+want_true, or NotEq+want_false -> match.
        jump_on_match = want_true_jump if is_eq else (not want_true_jump)
        fail_label = self.new_label("strcmp_fail") if jump_on_match else target

        # 1. Lengths must match first.
        len_op = self.compile_expr(len_arg, scope)
        len_reg = self._to_reg(len_op, scope)
        lit_len_reg = scope.alloc_temp()
        self.emit("LOD", _reg(lit_len_reg), _imm(len(literal)))
        self.emit("JNE", len_reg, _reg(lit_len_reg), _lbl(fail_label))
        scope.free_temp()
        _free_if_temp(len_reg, scope)

        # 2. Address into a scratch register we can offset per character
        #    without touching the caller's own variable.
        addr_op = self.compile_expr(addr_arg, scope)
        if addr_op[0] == "reg" and addr_op[1] in TEMP_REGS:
            addr_reg = addr_op[1]
        else:
            addr_reg = scope.alloc_temp()
            if addr_op[0] == "imm":
                self.emit("LOD", _reg(addr_reg), addr_op)
            else:
                self.emit_copy_preserving_src(addr_reg, addr_op[1])

        val_reg = scope.alloc_temp()
        char_reg = scope.alloc_temp()
        for i, ch in enumerate(literal):
            self.emit(read_ind, _reg(val_reg), _reg(addr_reg))
            self.emit_alu3("AND", _reg(val_reg), _reg(val_reg), _imm(0xFF), scope)
            self.emit("LOD", _reg(char_reg), _imm(ord(ch) & 0xFF))
            self.emit("JNE", _reg(val_reg), _reg(char_reg), _lbl(fail_label))
            if i < len(literal) - 1:
                self.emit("INC", _reg(addr_reg))
        scope.free_temp()  # char_reg
        scope.free_temp()  # val_reg
        scope.free_temp()  # addr_reg

        if jump_on_match:
            self.emit("JMP", _lbl(target))
            self.mark(fail_label)

    def compile_branch_on_false(self, test, scope, target):
        if isinstance(test, ast.Call) and isinstance(test.func, ast.Name) and test.func.id == "displayLoop":
            temp_reg = scope.alloc_temp()
            self.emit("LDM", _reg(temp_reg), _imm(0xFF))
            self.emit("JZ", _reg(temp_reg), _lbl(target))
            scope.free_temp()
            return

        if isinstance(test, ast.Call) and isinstance(test.func, ast.Name) and test.func.id == "halt":
            temp_reg = scope.alloc_temp()
            self.emit("LDM", _reg(temp_reg), _imm(0xFE))
            self.emit("JZ", _reg(temp_reg), _lbl(target))
            scope.free_temp()
            return

        match = _match_string_literal_compare(test)
        if match is not None:
            call, literal, is_eq = match
            self._compile_string_literal_branch(call, literal, is_eq, scope, target, want_true_jump=False)
            return

        if isinstance(test, ast.BoolOp) and isinstance(test.op, ast.And):
            for v in test.values:
                self.compile_branch_on_false(v, scope, target)
            return
        if isinstance(test, ast.BoolOp) and isinstance(test.op, ast.Or):
            skip = self.new_label("orskip")
            for v in test.values[:-1]:
                self.compile_branch_on_true(v, scope, skip)
            self.compile_branch_on_false(test.values[-1], scope, target)
            self.mark(skip)
            return
        if isinstance(test, ast.UnaryOp) and isinstance(test.op, ast.Not):
            self.compile_branch_on_true(test.operand, scope, target)
            return
        if isinstance(test, ast.Compare):
            self.compile_compare(test, scope, target, want_true_jump=False)
            return
        r = self.compile_expr(test, scope)
        if r[0] == "imm":
            r = self._imm_to_reg(r, scope)
        self.emit("JZ", r, _lbl(target))
        if r[1] in TEMP_REGS:
            scope.free_temp()

    def compile_branch_on_true(self, test, scope, target):
        match = _match_string_literal_compare(test)
        if match is not None:
            call, literal, is_eq = match
            self._compile_string_literal_branch(call, literal, is_eq, scope, target, want_true_jump=True)
            return

        if isinstance(test, ast.BoolOp) and isinstance(test.op, ast.Or):
            for v in test.values:
                self.compile_branch_on_true(v, scope, target)
            return
        if isinstance(test, ast.BoolOp) and isinstance(test.op, ast.And):
            skip = self.new_label("andskip")
            for v in test.values[:-1]:
                self.compile_branch_on_false(v, scope, skip)
            self.compile_branch_on_true(test.values[-1], scope, target)
            self.mark(skip)
            return
        if isinstance(test, ast.UnaryOp) and isinstance(test.op, ast.Not):
            self.compile_branch_on_false(test.operand, scope, target)
            return
        if isinstance(test, ast.Compare):
            self.compile_compare(test, scope, target, want_true_jump=True)
            return
        r = self.compile_expr(test, scope)
        if r[0] == "imm":
            r = self._imm_to_reg(r, scope)
        self.emit("JNZ", r, _lbl(target))
        if r[1] in TEMP_REGS:
            scope.free_temp()

    def compile_compare(self, node, scope, target, want_true_jump):
        if len(node.ops) != 1 or len(node.comparators) != 1:
            raise CompileError("chained comparisons (a < b < c) are not supported")
        op = type(node.ops[0])
        if op not in CMP_JUMP:
            raise CompileError(f"unsupported comparison {op}")
        true_op, false_op = CMP_JUMP[op]
        opcode = true_op if want_true_jump else false_op
        left = self.compile_expr(node.left, scope)
        right = self.compile_expr(node.comparators[0], scope)
        if left[0] == "imm":
            left = self._imm_to_reg(left, scope)
        if right[0] == "imm":
            right = self._imm_to_reg(right, scope)
        self.emit(opcode, left, right, _lbl(target))
        for r in (left, right):
            if r[0] == "reg" and r[1] in TEMP_REGS:
                scope.free_temp()

    def compile_expr_into(self, node, scope, dest_reg):
        if isinstance(node, ast.Call):
            folded = self._try_fold_const_expr(node)
            if folded is not None:
                self.emit("LOD", _reg(dest_reg), _imm(folded))
                return
            if self._try_intrinsic_expr(node, scope, dest_reg):
                return
            self.compile_call(node, scope, dest_reg)
            return
        r = self.compile_expr(node, scope)
        if r[0] == "imm":
            self.emit("LOD", _reg(dest_reg), r)
        elif r[0] == "reg":
            if r[1] != dest_reg:
                self.emit_copy(dest_reg, r[1])
            if r[1] in TEMP_REGS:
                scope.free_temp()
        else:
            raise CompileError("internal: bad operand kind")

    def compile_expr(self, node, scope):
        if isinstance(node, ast.Constant):
            if isinstance(node.value, bool): return _imm(1 if node.value else 0)
            if isinstance(node.value, int): return _imm(node.value)
            raise CompileError("only int/bool constants are supported")

        if isinstance(node, ast.Name):
            if node.id == "FRC_":
                dest = scope.alloc_temp()
                self.emit("LDM", _reg(dest), _imm(0xFD))
                return _reg(dest)
            return _reg(scope.var_reg(node.id))

        if isinstance(node, ast.Subscript):
            if not isinstance(node.value, ast.Name):
                raise CompileError("unsupported memory access")
            mem_name = node.value.id
            if mem_name in MEM_REGIONS:
                _, _, read_imm, _, read_ind = MEM_REGIONS[mem_name]
                slice_node = node.slice
                if hasattr(ast, 'Index') and isinstance(slice_node, ast.Index):
                    slice_node = slice_node.value
                addr_op = self.compile_expr(slice_node, scope)
                dest = scope.alloc_temp()
                if addr_op[0] == "imm":
                    self.emit(read_imm, _reg(dest), addr_op)
                else:
                    self.emit(read_ind, _reg(dest), addr_op)
                _free_if_temp(addr_op, scope)
                return _reg(dest)
            if hasattr(self, 'array_constants') and mem_name in self.array_constants:
                slice_node = node.slice
                idx_node = slice_node.value if hasattr(ast, 'Index') and isinstance(slice_node, ast.Index) else slice_node
                if isinstance(idx_node, ast.Constant):
                    return _imm(self.array_constants[mem_name][idx_node.value])
                raise CompileError("array subscript index must be a constant")
            raise CompileError(f"unknown memory region '{mem_name}'")

        if isinstance(node, ast.UnaryOp):
            src = self.compile_expr(node.operand, scope)
            src_r = src if src[0] == "reg" else self._imm_to_reg(src, scope)
            dest = _result_reg(src_r, scope)
            if isinstance(node.op, ast.USub):
                self.emit("NEG", _reg(dest), src_r)
            elif isinstance(node.op, ast.Invert):
                self.emit("NOT", _reg(dest), src_r)
            else:
                raise CompileError(f"unsupported unary op {node.op}")
            return _reg(dest)

        if isinstance(node, ast.BinOp):
            opcode = BINOP.get(type(node.op))
            if opcode is None: raise CompileError(f"unsupported operator {node.op}")
            left = self.compile_expr(node.left, scope)
            right = self.compile_expr(node.right, scope)
            dest = scope.alloc_temp()
            left_r = left if left[0] == "reg" else self._imm_to_reg(left, scope)
            if opcode in SHIFT_OPS:
                if right[0] != "imm":
                    raise CompileError(f"{opcode} amount must be a constant")
                self.emit(opcode, _reg(dest), left_r, right)
                _free_if_temp(left_r, scope)
            else:
                right_r = right if right[0] == "reg" else self._imm_to_reg(right, scope)
                self.emit(opcode, _reg(dest), left_r, right_r)
                _free_if_temp(left_r, scope)
                _free_if_temp(right_r, scope)
            return _reg(dest)

        if isinstance(node, ast.Call):
            # Compile-time fold note/freq → period so the .nu has a raw immediate
            folded = self._try_fold_const_expr(node)
            if folded is not None:
                return _imm(folded)
            dest = scope.alloc_temp()
            if self._try_intrinsic_expr(node, scope, dest):
                return _reg(dest)
            self.compile_call(node, scope, dest)
            return _reg(dest)

        raise CompileError(f"unsupported expression: {ast.dump(node)}")

    def _try_fold_const_expr(self, node):
        """If node is note_to_period("C4") or freq_to_period(440) with a constant
        argument, return the integer period; otherwise return None."""
        if not (isinstance(node, ast.Call) and isinstance(node.func, ast.Name)):
            return None
        fname = node.func.id
        if fname == "note_to_period":
            if len(node.args) != 1:
                raise CompileError("note_to_period(note) takes exactly 1 argument")
            arg = node.args[0]
            if not (isinstance(arg, ast.Constant) and isinstance(arg.value, str)):
                raise CompileError("note_to_period() requires a string literal note name (e.g. \"A4\")")
            return _note_to_period(arg.value)
        if fname == "freq_to_period":
            if len(node.args) != 1:
                raise CompileError("freq_to_period(freq) takes exactly 1 argument")
            arg = node.args[0]
            if not isinstance(arg, ast.Constant) or not isinstance(arg.value, (int, float)):
                raise CompileError("freq_to_period() requires a numeric constant")
            return _freq_to_period(float(arg.value))
        if fname == "note_to_freq":
            # Allowed for host-side use; in compiled code fold to period only via note_to_period
            if len(node.args) != 1:
                raise CompileError("note_to_freq(note) takes exactly 1 argument")
            arg = node.args[0]
            if not (isinstance(arg, ast.Constant) and isinstance(arg.value, str)):
                raise CompileError("note_to_freq() requires a string literal note name")
            # Return rounded Hz as int so it can be used as an immediate if needed
            return int(round(_note_to_freq(arg.value)))
        if fname == "len":
            # Arrays and strings have no runtime representation in this CPU, so
            # len() is always a compile-time operation. Return an immediate so
            # the caller emits only the constant load.
            if len(node.args) != 1:
                raise CompileError("len() takes exactly 1 argument")
            arg = node.args[0]
            if isinstance(arg, ast.Constant) and isinstance(arg.value, str):
                return len(arg.value)
            if isinstance(arg, ast.List):
                return len(arg.elts)
            if isinstance(arg, ast.Name) and hasattr(self, "array_constants"):
                if arg.id in self.array_constants:
                    return len(self.array_constants[arg.id])
            if isinstance(arg, ast.Name):
                raise CompileError(f"len() requires a compile-time array or string literal, got '{arg.id}'")
            raise CompileError(
                "len() requires a compile-time array or string literal"
            )
        return None

    def _try_intrinsic_expr(self, node, scope, dest_reg):
        if not (isinstance(node, ast.Call) and isinstance(node.func, ast.Name)):
            return False
        fname = node.func.id
        if fname == "readString":
            raise CompileError(
                "readString(...) can be used directly in a string argument "
                "(for example `execProgram(readString(\"VOLATILE\", addr, 8))`) "
                "or in a comparison with a string literal. It is not a general "
                "runtime string value."
            )
        if fname == "readArray":
            raise CompileError(
                "readArray(...) has no return value in compiled code (there's "
                "no runtime array type) - call it as a statement to print its "
                "contents, e.g. `readArray(region, addr, length)`"
            )
        if fname == "read":
            if len(node.args) != 2:
                raise CompileError("read(region, addr) takes exactly 2 arguments")
            region_arg, addr_arg = node.args

            if not (isinstance(region_arg, ast.Constant) and isinstance(region_arg.value, str)):
                raise CompileError("read region must be a string literal")

            mem_name = region_arg.value + "_"
            if mem_name not in MEM_REGIONS:
                raise CompileError(f"unknown region '{region_arg.value}'")
            _, _, read_imm, _, read_ind = MEM_REGIONS[mem_name]

            addr_op = self.compile_expr(addr_arg, scope)
            if addr_op[0] == "imm":
                self.emit(read_imm, _reg(dest_reg), addr_op)
            else:
                self.emit(read_ind, _reg(dest_reg), addr_op)

            _free_if_temp(addr_op, scope)
            return True

        if fname == "strFind":
            # strFind(region, addr, length, char) -> index of first match,
            # or `length` if the character is not present.
            if len(node.args) != 4:
                raise CompileError(
                    "strFind(region, addr, length, char) takes 4 arguments"
                )
            region_arg, addr_arg, len_arg, char_arg = node.args
            if not (isinstance(region_arg, ast.Constant) and isinstance(region_arg.value, str)):
                raise CompileError("strFind region must be a string literal")
            mem_name = region_arg.value + "_"
            if mem_name not in MEM_REGIONS:
                raise CompileError(f"unknown region '{region_arg.value}'")
            _, _, read_imm, _, read_ind = MEM_REGIONS[mem_name]

            addr_op = self.compile_expr(addr_arg, scope)
            len_op = self.compile_expr(len_arg, scope)
            char_op = self.compile_expr(char_arg, scope)

            # base address
            if addr_op[0] == "imm":
                base_r = scope.alloc_temp()
                self.emit("LOD", _reg(base_r), addr_op)
            elif addr_op[0] == "reg" and addr_op[1] in TEMP_REGS:
                base_r = addr_op[1]
            else:
                base_r = scope.alloc_temp()
                self.emit_copy_preserving_src(base_r, addr_op[1])

            # length into dest_reg as the default "not found" result
            if len_op[0] == "imm":
                self.emit("LOD", _reg(dest_reg), len_op)
            else:
                self.emit_copy_preserving_src(dest_reg, len_op[1])

            # needle character
            if char_op[0] == "imm":
                needle_r = scope.alloc_temp()
                self.emit("LOD", _reg(needle_r), char_op)
            elif char_op[0] == "reg" and char_op[1] in TEMP_REGS:
                needle_r = char_op[1]
            else:
                needle_r = scope.alloc_temp()
                self.emit_copy_preserving_src(needle_r, char_op[1])

            i_r = scope.alloc_temp()
            self.emit("LOD", _reg(i_r), _imm(0))

            find_top = self.new_label("strfind_top")
            find_done = self.new_label("strfind_done")
            find_hit = self.new_label("strfind_hit")

            self.mark(find_top)
            self.emit("JAE", _reg(i_r), _reg(dest_reg), _lbl(find_done))
            ch_r = scope.alloc_temp()
            addr_tmp = scope.alloc_temp()
            self.emit("ADD", _reg(addr_tmp), _reg(base_r), _reg(i_r))
            self.emit(read_ind, _reg(ch_r), _reg(addr_tmp))
            scope.free_temp()  # addr_tmp
            self.emit_alu3("AND", _reg(ch_r), _reg(ch_r), _imm(0xFF), scope)
            self.emit("JE", _reg(ch_r), _reg(needle_r), _lbl(find_hit))
            scope.free_temp()  # ch_r
            self.emit("INC", _reg(i_r))
            self.emit("JMP", _lbl(find_top))

            self.mark(find_hit)
            scope.free_temp()  # ch_r (still live on the JE path)
            self.emit_copy_preserving_src(dest_reg, i_r)

            self.mark(find_done)
            scope.free_temp()  # i_r
            if not (char_op[0] == "reg" and char_op[1] in TEMP_REGS):
                scope.free_temp()  # needle_r
            if not (addr_op[0] == "reg" and addr_op[1] in TEMP_REGS):
                scope.free_temp()  # base_r
            _free_if_temp(len_op, scope)
            return True

        if fname == "diskStat":
            # diskStat(name) -> length in words, or 0xFFFF if not found.
            if len(node.args) != 1:
                raise CompileError("diskStat(name) takes exactly 1 argument")
            name_arg = node.args[0]
            self._emit_disk_name_arg(name_arg, scope)
            self._emit_disk_trigger(DISK_CMD_STAT, scope)
            self.emit("LDM", _reg(dest_reg), _imm(DISK_RESULT))
            return True

        if fname == "diskLoad":
            # diskLoad(name, region, addr) -> words actually loaded, or
            # 0xFFFF if the file wasn't found. region is "VOLATILE",
            # "DISK", or "VRAM".
            if len(node.args) != 3:
                raise CompileError("diskLoad(name, region, addr) takes exactly 3 arguments")
            name_arg, region_arg, addr_arg = node.args
            if not (isinstance(region_arg, ast.Constant) and isinstance(region_arg.value, str)):
                raise CompileError("diskLoad region must be a string literal")
            self._emit_disk_name_arg(name_arg, scope)
            addr_op = self.compile_expr(addr_arg, scope)
            addr_reg = self._to_reg(addr_op, scope)
            self.emit("MOV", addr_reg, _imm(DISK_ADDR))
            _free_if_temp(addr_reg, scope)
            self._emit_disk_region(region_arg.value, scope)
            self._emit_disk_trigger(DISK_CMD_LOAD, scope)
            self.emit("LDM", _reg(dest_reg), _imm(DISK_RESULT))
            return True

        if fname == "getPixel":
            if len(node.args) != 2:
                raise CompileError("getPixel(x, y) takes exactly 2 arguments")
            x_op, y_op = [self.compile_expr(a, scope) for a in node.args]
            self.emit_gpu_command(GPU_CMD_GETPIXEL, [
                (GPU_X0, x_op), (GPU_Y0, y_op),
            ], scope)
            self.emit("LDM", _reg(dest_reg), _imm(GPU_RESULT))
            return True
        return False

    def _imm_to_reg(self, imm_operand, scope):
        r = scope.alloc_temp()
        self.emit("LOD", _reg(r), imm_operand)
        return _reg(r)

    def _emit_write_words(self, write_imm, write_ind, start_addr_op, vals, scope):
        """Write `vals` (a list of int constants) sequentially starting at
        start_addr_op. Shared by writeArray and writeString."""
        if start_addr_op[0] == "imm":
            start_val = start_addr_op[1]
            for i, val in enumerate(vals):
                val_reg = scope.alloc_temp()
                self.emit("LOD", _reg(val_reg), _imm(val))
                self.emit(write_imm, _reg(val_reg), _imm(start_val + i))
                scope.free_temp()
            return

        # Never increment the caller's own register in place - if
        # start_addr_op isn't already a scratch temp, copy it into one first.
        if start_addr_op[0] == "reg" and start_addr_op[1] in TEMP_REGS:
            addr_reg = start_addr_op[1]
        else:
            addr_reg = scope.alloc_temp()
            self.emit_copy_preserving_src(addr_reg, start_addr_op[1])

        for i, val in enumerate(vals):
            val_reg = scope.alloc_temp()
            self.emit("LOD", _reg(val_reg), _imm(val))
            self.emit(write_ind, _reg(val_reg), _reg(addr_reg))
            if i < len(vals) - 1:
                self.emit("INC", _reg(addr_reg))
            scope.free_temp()
        scope.free_temp()  # addr_reg

    def compile_call(self, node, scope, dest_reg):
        if not isinstance(node.func, ast.Name) or node.func.id not in self.funcs:
            fname = node.func.id if isinstance(node.func, ast.Name) else "?"
            raise CompileError(f"call to unknown function '{fname}'")
        if node.func.id in self.call_stack:
            raise CompileError(
                f"'{node.func.id}' calls itself (directly or indirectly)"
            )
        info = self.funcs[node.func.id]
        if len(node.args) != len(info.params):
            raise CompileError(
                f"'{node.func.id}' expects {len(info.params)} args, got {len(node.args)}"
            )

        arg_temps = [self.compile_expr(a, scope) for a in node.args]
        arg_regs = []
        for a in arg_temps:
            arg_regs.append(a if a[0] == "reg" else self._imm_to_reg(a, scope))

        # Save every live register (including any temps holding immediate
        # args) to VOLATILE *before* touching any registers with SWP below,
        # so the archived values are the real ones, not ones already
        # clobbered by the argument shuffle.
        live = sorted(set(scope.vars.values()) | set(TEMP_REGS[:scope.temp_sp]))
        save_base = self.alloc_save_slots(len(live)) if live else 0
        for i, r in enumerate(live):
            self.emit("MOV", _reg(r), _imm(save_base + i))

        # Stage every arg into a temp register before moving it into its
        # final R0..Rn-1 slot. Moving args directly into place with SWP is
        # destructive when an arg's source register overlaps another arg's
        # target slot (e.g. calling f(key, cursor_x, cursor_y) when
        # key/cursor_x/cursor_y already live in R0-R2) - placing one arg
        # clobbers the value the next arg still needs to read. Temps are
        # disjoint from the target slots, so this copy can't be corrupted
        # by the moves that follow it. Immediates are already sitting in a
        # temp from _imm_to_reg above, so reuse that one instead of
        # allocating a second temp for them. It's safe to clobber the
        # source var registers here since their true values are already
        # archived above.
        staged = []
        for r in arg_regs:
            if r[1] in TEMP_REGS:
                # Already a temp (from _imm_to_reg above) - that alloc_temp()
                # call is still outstanding and needs freeing below, same as
                # the freshly-allocated ones.
                staged.append(r[1])
            else:
                t = scope.alloc_temp()
                self.emit_copy(t, r[1])
                staged.append(t)

        for i, t in enumerate(staged):
            if t != i:
                self.emit_copy(i, t)
        for _ in staged:
            scope.free_temp()

        self.emit("CAL", _lbl(info.start_label))
        for i, r in enumerate(live):
            self.emit("LDM", _reg(r), _imm(save_base + i))

        if dest_reg is not None and dest_reg != RET_REG:
            self.emit_copy(dest_reg, RET_REG)


def render_operand(op):
    kind, val = op
    if kind == "reg":
        return f"R{val:02X}"
    if kind == "imm":
        return f"{val:04X}"
    if kind == "label":
        return val
    raise CompileError(f"bad operand {op}")


def assemble(instrs):
    addr = 0
    label_addr = {}
    sized = []
    for item in instrs:
        if item[0] == "LABEL":
            label_addr[item[1]] = addr
            continue
        if item[0] == "COMMENT":
            # Comments do not occupy addresses; keep them interleaved
            sized.append(("COMMENT", item[1]))
            continue
        mnemonic, operands = item
        sized.append((addr, mnemonic, operands))
        addr += 1 + OPERAND_COUNTS[mnemonic]

    out = []
    for entry in sized:
        if entry[0] == "COMMENT":
            out.append(("COMMENT", entry[1]))
            continue
        _, mnemonic, operands = entry
        rendered = []
        for op in operands:
            if op[0] == "label":
                if op[1] not in label_addr:
                    raise CompileError(f"undefined label '{op[1]}'")
                target = label_addr[op[1]]
                rendered.append(f"{target:04X}")
            else:
                rendered.append(render_operand(op))
        # Return original operands alongside rendered ones for comment generation
        out.append((mnemonic, rendered, operands))
    return out


def to_nu_text(assembled):
    lines = ["; NuevoAuto assembly code auto-generated by compile.py\n"]
    for item in assembled:
        if item[0] == "COMMENT":
            # Free-form source-derived comments (function headers, docstrings)
            text = item[1]
            if text:
                lines.append(f"; {text}")
            else:
                lines.append(";")
            continue
        mnemonic, rendered, original = item
        line = " ".join([mnemonic] + rendered)
        comment = generate_comment(mnemonic, original, rendered)
        if comment:
            line = f"{line} ; {comment}"
        lines.append(line)
    return "\n".join(lines) + "\n"


def assemble_words(instrs):
    """Resolve labels and flatten `instrs` into a list of raw 16-bit words:
    [opcode_index, operand, operand, ...] per instruction, back to back -
    exactly the layout NPROG expects at runtime. COMMENT/LABEL pseudo-ops
    are skipped (labels only affect addresses, comments don't exist in
    binary output).
    """
    addr = 0
    label_addr = {}
    real = []
    for item in instrs:
        if item[0] == "LABEL":
            label_addr[item[1]] = addr
            continue
        if item[0] == "COMMENT":
            continue
        mnemonic, operands = item
        real.append((mnemonic, operands))
        addr += 1 + OPERAND_COUNTS[mnemonic]

    words = []
    for mnemonic, operands in real:
        words.append(OPCODE_INDEX[mnemonic])
        for kind, val in operands:
            if kind == "label":
                if val not in label_addr:
                    raise CompileError(f"undefined label '{val}'")
                words.append(label_addr[val] & 0xFFFF)
            else:
                words.append(val & 0xFFFF)
    return words


def words_to_bytes(words):
    """Pack a list of 16-bit words into a little-endian byte stream -
    the format Memory::loadReadonlyFile / loadNprogFile expect."""
    out = bytearray(len(words) * 2)
    for i, w in enumerate(words):
        out[i * 2] = w & 0xFF
        out[i * 2 + 1] = (w >> 8) & 0xFF
    return bytes(out)


def compile_source_binary(source):
    """Compile nupy source straight to a little-endian uint16 byte stream
    (no text .nu step). This is what mkdisk.py packs into disk entries."""
    tree = ast.parse(source)
    c = Compiler()
    c.compile_module(tree)
    words = assemble_words(c.instrs)
    return words_to_bytes(words)


def compile_file_binary(py_path, nub_path=None):
    with open(py_path) as f:
        source = f.read()
    data = compile_source_binary(source)
    nub_path = nub_path or (py_path.rsplit(".", 1)[0] + ".nub")
    with open(nub_path, "wb") as f:
        f.write(data)
    return nub_path


def load_nu(path):
    program = []
    with open(path) as f:
        for line in f:
            line = line.split(";", 1)[0].strip()
            if not line:
                continue
            parts = line.split()
            mnemonic, operands = parts[0], parts[1:]
            operands = [o[1:] if o.startswith("R") else o for o in operands]
            program.append((mnemonic, *operands))
    return program

def compile_source(source):
    tree = ast.parse(source)
    c = Compiler()
    c.compile_module(tree)
    assembled = assemble(c.instrs)
    return to_nu_text(assembled)

def compile_file(py_path, nu_path=None):
    with open(py_path) as f:
        source = f.read()
    nu_text = compile_source(source)
    nu_path = nu_path or (py_path.rsplit(".", 1)[0] + ".nu")
    with open(nu_path, "w") as f:
        f.write(nu_text)
    return nu_path

if __name__ == "__main__":
    args = sys.argv[1:]
    binary = "--bin" in args
    if binary:
        args.remove("--bin")

    dir_mode = "--dir" in args
    if dir_mode:
        args.remove("--dir")

    output_dir = None
    if "--output" in args:
        idx = args.index("--output")
        if idx + 1 >= len(args):
            print("error: --output requires a directory argument")
            raise SystemExit(1)
        output_dir = args.pop(idx + 1)
        args.pop(idx)

    if len(args) != 1:
        print("usage: python compile.py [--bin] [--dir] [--output <dir>] <path>")
        print("  --bin         emit raw uint16 .nub binary instead of .nu text")
        print("  --dir         batch compile all .nupy files in directory")
        print("  --output DIR  write output files to DIR instead of source directory")
        raise SystemExit(1)

    try:
        if dir_mode:
            # Batch compile directory
            import os
            import glob
            directory = args[0]
            pattern = os.path.join(directory, "**/*.nupy")
            files = glob.glob(pattern, recursive=True)
            if not files:
                print(f"no .nupy files found in {directory}")
                raise SystemExit(1)

            if output_dir and not os.path.exists(output_dir):
                os.makedirs(output_dir, exist_ok=True)

            for py_path in sorted(files):
                try:
                    if output_dir:
                        base_name = os.path.basename(py_path)
                        out_name = base_name.rsplit(".", 1)[0]
                        out_ext = ".nub" if binary else ".nu"
                        out_path = os.path.join(output_dir, out_name + out_ext)
                        if binary:
                            data = compile_source_binary(open(py_path).read())
                            with open(out_path, "wb") as f:
                                f.write(data)
                        else:
                            source = open(py_path).read()
                            nu_text = compile_source(source)
                            with open(out_path, "w") as f:
                                f.write(nu_text)
                    else:
                        out_path = compile_file_binary(py_path) if binary else compile_file(py_path)
                    print(f"Success: {out_path}")
                except CompileError as e:
                    print(f"Fail: {py_path}: {e}")
        else:
            # Single file compile
            if output_dir:
                base_name = os.path.basename(args[0])
                out_name = base_name.rsplit(".", 1)[0]
                out_ext = ".nub" if binary else ".nu"
                out_path = os.path.join(output_dir, out_name + out_ext)
                if binary:
                    compile_file_binary(args[0], out_path)
                else:
                    compile_file(args[0], out_path)
            else:
                out_path = compile_file_binary(args[0]) if binary else compile_file(args[0])
            print(f"wrote {out_path}")
    except CompileError as e:
        print(f"compile error: {e}")
        raise SystemExit(1)