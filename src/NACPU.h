//
// Created by rwarr on 8/7/2026.
//

#ifndef NUEVOAUTO_NACPU_H
#define NUEVOAUTO_NACPU_H

#include <array>
#include <unordered_map>
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <iostream>

#include "memory.h"
#include "november.h"

#define GPU_CMD    0xFFF0
#define GPU_X0     0xFFF1
#define GPU_Y0     0xFFF2
#define GPU_X1     0xFFF3
#define GPU_Y1     0xFFF4
#define GPU_COLOR  0xFFF5
#define GPU_RESULT 0xFFF6

#define GPU_CMD_SETPIXEL 1
#define GPU_CMD_GETPIXEL 2
#define GPU_CMD_DRAWLINE 3

// Disk controller port block - mirrors the GPU_CMD pattern above. Writing
// DISK_CMD triggers the operation once every other register is set.
// DISK_NAMEPTR points at an 8-word packed filename buffer in VOLATILE
// (same packing writeString() already uses elsewhere).
#define DISK_CMD     0xFFE0
#define DISK_NAMEPTR 0xFFE1
#define DISK_REGION  0xFFE2  // 0=VOLATILE 1=DISK 2=VRAM 3=NPROG (dest/src for LOAD/SAVE)
#define DISK_ADDR    0xFFE3  // dest/src address within DISK_REGION
#define DISK_LEN     0xFFE4  // word count, used by SAVE
#define DISK_RESULT  0xFFE5  // status/length result, written by the host

#define DISK_CMD_STAT   1  // -> DISK_RESULT = length in words, or 0xFFFF if not found
#define DISK_CMD_LOAD   2  // load named file into DISK_REGION:DISK_ADDR -> DISK_RESULT = length, 0xFFFF on failure
#define DISK_CMD_SAVE   3  // save DISK_LEN words from DISK_REGION:DISK_ADDR under the name -> DISK_RESULT = 1/0
#define DISK_CMD_DELETE 4  // -> DISK_RESULT = 1/0
#define DISK_CMD_EXEC   5  // chain-load: load named file into NPROG and jump to it (does not return)

#define STACK_TOP          0x7F00
#define STACK_LIMIT        0x7000

#define IRQ_PENDING        0x00F0
#define IRQ_MASK           0x00F1
#define IRQ_STATUS         0x00F2
#define IRQ_TIMER_RELOAD   0x00F3
#define IRQ_TIMER_COUNT    0x00F4
#define IRQ_VECTOR_BASE    0x00F8

#define IRQ_VBLANK 0
#define IRQ_INPUT  1
#define IRQ_TIMER  2
#define IRQ_SOFT   3
#define IRQ_SOURCE_COUNT 8

constexpr std::array<std::string_view, 65> OPCODES = {
    "NOP", // No-op
    "MOV", // Copy register to volatile memory address
    "SMV", // Copy register to save memory (DISK) address
    "VMV", // Copy register to VRAM address
    "LDM", // Load register from volatile memory address
    "LOD", // Load value into register
    "SWP", // Swap source/destination operands
    "ADD", // Add registers
    "ADC", // Add with carry
    "SUB", // Subtract registers
    "SBB", // Subtract with borrow
    "INC", // Increment register
    "DEC", // Decrement register
    "MUL", // Multiply registers
    "DIV", // Divide registers
    "AND", // Bitwise AND
    "OR", // Bitwise OR
    "XOR", // Bitwise Exclusive OR
    "NOT", // Logical NOT
    "NEG", // Negate register, 2's complement
    "SHL", // Shift Logical Left
    "SAL", // Shift Arithemtic Left
    "SHR", // Shift Logical Right
    "SAR", // Shift Arithmetic Right
    "JMP", // Unconditional Jump to address
    "JE", // Jump if equal
    "JNE", // Jump if not equal
    "JZ", // Jump if zero
    "JNZ", // Jump if not zero
    "JL", // Jump if less than (signed)
    "JLE", // Jump if less than or equal (signed)
    "JG", // Jump if greater than (signed)
    "JGE", // Jump if greater than or equal (signed)
    "JB", // Jump if below (unsigned <)
    "JBE", // Jump if below or equal (unsigned <=)
    "JA", // Jump if above (unsigned >)
    "JAE", // Jump if above or equal (unsigned >=)
    "CAL", // Call subroutine (push PC onto VOLATILE stack, jump)
    "RET", // Return from subroutine (pop PC)
    "FOR", // loop, operands: counter_reg, limit, addr
    "BRK", // Break execution
    "PRT", // Print register (debug)
    "LSM", // Load register from DISK address
    "LVM", // Load register from VRAM address
    "LNP", // Load register from NPROG address
    "SNP", // Copy register to NPROG address
    "FLP", // Flip display buffers (update the screen)

    // Register-indirect memory ops (address comes from a register)
    "MOVI",  // src_reg, addr_reg  -> VOLATILE[regs[addr_reg]] = regs[src_reg]
    "SMVI",  // src_reg, addr_reg  -> DISK[regs[addr_reg]]
    "VMVI",  // src_reg, addr_reg  -> VRAM[regs[addr_reg]]  (0xFFFF still clears)
    "LDMI",  // dest_reg, addr_reg <- VOLATILE[regs[addr_reg]]
    "LSMI",  // dest_reg, addr_reg <- DISK
    "LVMI",  // dest_reg, addr_reg <- VRAM
    "LNPI",  // dest_reg, addr_reg <- NPROG
    "SNPI",  // src_reg, addr_reg  -> NPROG[regs[addr_reg]]
    "LBM",   // dest_reg, addr      <- SYSROM (read-only)
    "LBMI",  // dest_reg, addr_reg  <- SYSROM (read-only)

    // Stack / interrupt control
    "PSH",   // push reg onto VOLATILE stack (via SP)
    "POP",   // pop VOLATILE stack into reg
    "LSP",   // load SP from reg (SP := regs[src])
    "SSP",   // store SP into reg (regs[dest] := SP)
    "SEI",   // set interrupt-enable flag (global)
    "CLI",   // clear interrupt-enable flag (global)
    "RTI",   // return from interrupt (pop status, pop PC)
    "SWI",   // software interrupt (raise IRQ_SOFT bit)
};

constexpr std::array<std::string_view, 5> REGION_CODES = {
    "VOLATILE", "DISK", "VRAM", "NPROG", "SYSROM"
};

// Map from Python-style region names used in the ISA to Memory map keys.
inline const char* regionKey(std::string_view pyName) {
    if (pyName == "VOLATILE") return "volatile";
    if (pyName == "DISK")     return "disk";
    if (pyName == "VRAM")     return "vram";
    if (pyName == "NPROG")    return "nprog";
    if (pyName == "SYSROM")   return "sysrom";
    return "volatile"; // fallback
}

const std::unordered_map<std::string_view, int> OPERAND_COUNTS = {
    {"NOP", 0},
    {"MOV", 2},
    {"SMV", 2},
    {"VMV", 2},
    {"LDM", 2},
    {"LOD", 2},
    {"SWP", 2},
    {"ADD", 3},
    {"ADC", 3},
    {"SUB", 3},
    {"SBB", 3},
    {"INC", 1},
    {"DEC", 1},
    {"MUL", 3},
    {"DIV", 3},
    {"AND", 3},
    {"OR", 3},
    {"XOR", 3},
    {"NOT", 2},
    {"NEG", 2},
    {"SHL", 3},
    {"SAL", 3},
    {"SHR", 3},
    {"SAR", 3},
    {"JMP", 1},
    {"JE", 3},
    {"JNE", 3},
    {"JZ", 2},
    {"JNZ", 2},
    {"JL", 3},
    {"JLE", 3},
    {"JG", 3},
    {"JGE", 3},
    {"JB", 3},
    {"JBE", 3},
    {"JA", 3},
    {"JAE", 3},
    {"CAL", 1},
    {"RET", 0},
    {"FOR", 3},
    {"BRK", 0},
    {"PRT", 1},
    {"LSM", 2},
    {"LVM", 2},
    {"LNP", 2},
    {"SNP", 2},
    {"FLP", 0},
    {"MOVI", 2},
    {"SMVI", 2},
    {"VMVI", 2},
    {"LDMI", 2},
    {"LSMI", 2},
    {"LVMI", 2},
    {"LNPI", 2},
    {"SNPI", 2},
    {"LBM", 2},
    {"LBMI", 2},
    {"PSH", 1},
    {"POP", 1},
    {"LSP", 1},
    {"SSP", 1},
    {"SEI", 0},
    {"CLI", 0},
    {"RTI", 0},
    {"SWI", 0},
};

struct Instruction {
    std::string opcode_name;
    std::vector<int> operands;
};

class NACPU {
public:
    // Memory and November are large; take references (caller owns lifetime).
    NACPU(Memory& mem, November& nov);

    void raiseIrq(int source);
    void checkInterrupts();
    void updateHardwareFlags();
    void signalVblank();

    // program is a sequence of instructions: each entry is
    // {opcode_name, op0, op1, ...} where operands may be decimal or 0x-hex.
    void loadProgram(const std::vector<std::vector<std::string>>& program);
    void loadProgram(const std::vector<Instruction>& program);

    // Load a .nu assembly text file (compile.py output format).
    // Handles: ; comments, R00-R0F register names, bare hex immediates,
    // blank lines. Returns true on success.
    bool loadNuFile(const std::string& path);

    // Parse a .nu source string into Instruction list (does not load into memory).
    static std::vector<Instruction> parseNu(const std::string& source);

    // Finds the disk's bootable entry, loads its raw .nub bytes into NPROG,
    // and resets PC/registers to start it. Returns false (machine does not
    // boot) if no bootable entry exists. This is the disk-based counterpart
    // to loadNuFile - use one or the other, not both.
    bool bootFromDisk();

    Instruction fetch();
    void run();
    void operate(const Instruction& instr);

    // Accessors useful for the host / debugger
    bool isRunning() const { return running; }
    int  getPC() const { return pc; }
    int  getSP() const { return sp; }
    bool getCarry() const { return carry; }
    const std::array<int64_t, 16>& getRegisters() const { return registers; }

private:
    Memory& memory;
    November& display;

    std::array<int64_t, 16> registers{};
    int sp = STACK_TOP;
    int pc = 0;
    bool running = true;
    int program_end = 0;
    bool carry = false;
    bool ie = false;
    bool in_isr = false;
    int prev_input_sig = 0;
    bool timer_armed = false;
    int host_frame = 0;   // local mirror; November::frame is private

    static int wrap(int value);
    static int signed16(int value);

    // Bounds-checked register access. Invalid indices return 0 / no-op so a
    // malformed program cannot assert on std::array::operator[] in debug builds.
    int64_t getReg(int reg) const;
    void set(int reg, int64_t value);
    void push(int64_t value);
    int  pop();

    int  packStatus();
    void unpackStatus(int s);
    void updateIrqStatusMirror();
    void clearIrqBit(int source);
    void pollTimer();
    void pollInputEdge();

    void gpuDispatch();
    void diskDispatch();

    // Installs `words` as the running program: copies into NPROG, resets
    // PC to 0 and program_end to the new length. Used by both bootFromDisk()
    // and the EXEC disk command (chain-load) - the only difference between
    // the two is where the bytes came from and whether registers are reset.
    void installProgram(const std::vector<uint16_t>& words);

    // Reads the 8-word packed filename at VOLATILE[DISK_NAMEPTR] into a string.
    std::string readDiskNamePtr();

    // Convenience wrappers that map ISA region names -> Memory keys
    void memWrite(const char* pyRegion, size_t addr, uint16_t val);
    uint16_t memRead(const char* pyRegion, size_t addr);

    // Operand parsing helpers for .nu assembly
    static int parseOperand(const std::string& token);
    static bool isRegisterToken(const std::string& token);
};

#endif //NUEVOAUTO_NACPU_H
