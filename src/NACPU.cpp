//
// Created by rwarr on 8/7/2026.
// Full port of the Python NACPU implementation, wired to the real November class.
//

#include "NACPU.h"

#include <stdexcept>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cctype>

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
NACPU::NACPU(Memory& mem, November& nov)
    : memory(mem), display(nov)
{
    // Initialise IRQ control registers (mirrors Python __init__)
    memWrite("VOLATILE", IRQ_PENDING, 0);
    memWrite("VOLATILE", IRQ_MASK, 0);
    memWrite("VOLATILE", IRQ_STATUS, 0);
    memWrite("VOLATILE", IRQ_TIMER_RELOAD, 0);
    memWrite("VOLATILE", IRQ_TIMER_COUNT, 0);
    for (int i = 0; i < IRQ_SOURCE_COUNT; ++i) {
        memWrite("VOLATILE", IRQ_VECTOR_BASE + i, 0);
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
int NACPU::wrap(int value) {
    return static_cast<int>(static_cast<uint16_t>(value));
}

int NACPU::signed16(int value) {
    int v = wrap(value);
    return (v >= 32768) ? v - 65536 : v;
}

int64_t NACPU::getReg(int reg) const {
    if (reg < 0 || reg >= 16) return 0;
    return registers[static_cast<size_t>(reg)];
}

void NACPU::set(int reg, int64_t value) {
    if (reg < 0 || reg >= 16) return;
    registers[static_cast<size_t>(reg)] = wrap(static_cast<int>(value));
}

void NACPU::memWrite(const char* pyRegion, size_t addr, uint16_t val) {
    memory.write16(regionKey(pyRegion), addr, val);
}

uint16_t NACPU::memRead(const char* pyRegion, size_t addr) {
    return memory.read16(regionKey(pyRegion), addr);
}

void NACPU::push(int64_t value) {
    sp = wrap(sp - 1);
    if (sp < STACK_LIMIT) {
        std::cerr << "STACK UNDERFLOW: SP=" << std::hex << sp << std::dec << '\n';
        running = false;
        return;
    }
    memWrite("VOLATILE", static_cast<size_t>(sp),
             static_cast<uint16_t>(wrap(static_cast<int>(value))));
}

int NACPU::pop() {
    if (sp >= STACK_TOP) {
        std::cerr << "STACK OVERFLOW/EMPTY: SP=" << std::hex << sp << std::dec << '\n';
        running = false;
        return 0;
    }
    int val = memRead("VOLATILE", static_cast<size_t>(sp));
    sp = wrap(sp + 1);
    return val;
}

int NACPU::packStatus() {
    int s = 0;
    if (ie)     s |= 0x0001;
    if (carry)  s |= 0x0002;
    if (in_isr) s |= 0x0004;
    return s;
}

void NACPU::unpackStatus(int s) {
    ie    = (s & 0x0001) != 0;
    carry = (s & 0x0002) != 0;
    // in_isr is restored by the RTI path itself
}

void NACPU::updateIrqStatusMirror() {
    int s = 0;
    if (ie)     s |= 0x0001;
    if (in_isr) s |= 0x0002;
    memWrite("VOLATILE", IRQ_STATUS, static_cast<uint16_t>(s));
}

void NACPU::raiseIrq(int source) {
    if (source < 0 || source >= IRQ_SOURCE_COUNT) return;
    uint16_t pending = memRead("VOLATILE", IRQ_PENDING);
    pending |= static_cast<uint16_t>(1 << source);
    memWrite("VOLATILE", IRQ_PENDING, pending);
}

void NACPU::clearIrqBit(int source) {
    uint16_t pending = memRead("VOLATILE", IRQ_PENDING);
    pending &= static_cast<uint16_t>(~(1 << source));
    memWrite("VOLATILE", IRQ_PENDING, static_cast<uint16_t>(wrap(pending)));
}

void NACPU::pollTimer() {
    uint16_t reload = memRead("VOLATILE", IRQ_TIMER_RELOAD);
    if (reload == 0) {
        timer_armed = false;
        return;
    }
    uint16_t count = memRead("VOLATILE", IRQ_TIMER_COUNT);
    if (!timer_armed) {
        memWrite("VOLATILE", IRQ_TIMER_COUNT, reload);
        timer_armed = true;
        return;
    }
    if (count == 0) {
        raiseIrq(IRQ_TIMER);
        memWrite("VOLATILE", IRQ_TIMER_COUNT, reload);
    } else {
        memWrite("VOLATILE", IRQ_TIMER_COUNT, static_cast<uint16_t>(count - 1));
    }
}

void NACPU::pollInputEdge() {
    // Same four ports the Python core watches
    int sig = memRead("VOLATILE", 0x8000)
            ^ memRead("VOLATILE", 0x8010)
            ^ memRead("VOLATILE", 0x8011)
            ^ memRead("VOLATILE", 0x8015);
    if (sig != prev_input_sig && prev_input_sig != 0) {
        raiseIrq(IRQ_INPUT);
    }
    prev_input_sig = sig;
}

void NACPU::checkInterrupts() {
    if (!ie || in_isr || !running) return;

    uint16_t pending = memRead("VOLATILE", IRQ_PENDING);
    uint16_t mask    = memRead("VOLATILE", IRQ_MASK);
    uint16_t active  = pending & mask;
    if (active == 0) return;

    int source = 0;
    while (source < IRQ_SOURCE_COUNT && !(active & (1 << source))) {
        ++source;
    }
    if (source >= IRQ_SOURCE_COUNT) return;

    uint16_t vector = memRead("VOLATILE", IRQ_VECTOR_BASE + source);
    if (vector == 0) {
        clearIrqBit(source);
        return;
    }

    clearIrqBit(source);
    push(pc);
    push(packStatus());
    in_isr = true;
    ie = false;
    pc = vector;
    updateIrqStatusMirror();
}

void NACPU::gpuDispatch() {
    uint16_t cmd = memRead("VOLATILE", GPU_CMD);
    if (cmd == GPU_CMD_SETPIXEL) {
        int x = memRead("VOLATILE", GPU_X0);
        int y = memRead("VOLATILE", GPU_Y0);
        int color = memRead("VOLATILE", GPU_COLOR);
        display.drawPixel(x, y, color);
    } else if (cmd == GPU_CMD_GETPIXEL) {
        int x = memRead("VOLATILE", GPU_X0);
        int y = memRead("VOLATILE", GPU_Y0);
        memWrite("VOLATILE", GPU_RESULT, static_cast<uint16_t>(display.getPixel(x, y)));
    } else if (cmd == GPU_CMD_DRAWLINE) {
        int x0 = memRead("VOLATILE", GPU_X0);
        int y0 = memRead("VOLATILE", GPU_Y0);
        int x1 = memRead("VOLATILE", GPU_X1);
        int y1 = memRead("VOLATILE", GPU_Y1);
        int color = memRead("VOLATILE", GPU_COLOR);
        display.drawLine(x0, y0, x1, y1, color);
    }
}

std::string NACPU::readDiskNamePtr() {
    int ptr = memRead("VOLATILE", DISK_NAMEPTR);
    std::string name;
    for (int i = 0; i < 8; ++i) {
        uint16_t w = memRead("VOLATILE", static_cast<size_t>(ptr + i));
        if ((w & 0xFF) == 0) break;
        name += static_cast<char>(w & 0xFF);
    }
    return name;
}

void NACPU::installProgram(const std::vector<uint16_t>& words) {
    memory.loadNprog(words.data(), words.size());
    program_end = static_cast<int>(words.size());
    pc = 0;
}

// Region codes for DISK_REGION, shared by LOAD/SAVE. Only RAM-backed regions
// make sense here - SYSROM is read-only and NPROG has its own EXEC path.
namespace {
    const char* diskRegionName(int code) {
        switch (code) {
            case 0: return "VOLATILE";
            case 1: return "DISK";
            case 2: return "VRAM";
            case 3: return "NPROG";
            default: return nullptr;
        }
    }
}

void NACPU::diskDispatch() {
    uint16_t cmd = memRead("VOLATILE", DISK_CMD);

    if (cmd == DISK_CMD_STAT) {
        std::string name = readDiskNamePtr();
        DiskEntry entry;
        uint16_t result = memory.diskFindEntry(name, entry)
            ? static_cast<uint16_t>(entry.length & 0xFFFF) : 0xFFFF;
        memWrite("VOLATILE", DISK_RESULT, result);
    }
    else if (cmd == DISK_CMD_LOAD) {
        std::string name = readDiskNamePtr();
        int regionCode = memRead("VOLATILE", DISK_REGION);
        int addr = memRead("VOLATILE", DISK_ADDR);
        const char* region = diskRegionName(regionCode);

        DiskEntry entry;
        std::vector<uint16_t> words;
        if (!region || !memory.diskFindEntry(name, entry) ||
            !memory.diskReadEntryWords(entry, words)) {
            memWrite("VOLATILE", DISK_RESULT, 0xFFFF);
            return;
        }
        memory.write16Array(regionKey(region), static_cast<size_t>(addr),
                             words.data(), words.size());
        memWrite("VOLATILE", DISK_RESULT, static_cast<uint16_t>(words.size() & 0xFFFF));
    }
    else if (cmd == DISK_CMD_SAVE) {
        std::string name = readDiskNamePtr();
        int regionCode = memRead("VOLATILE", DISK_REGION);
        int addr = memRead("VOLATILE", DISK_ADDR);
        int len = memRead("VOLATILE", DISK_LEN);
        const char* region = diskRegionName(regionCode);

        if (!region || len <= 0) {
            memWrite("VOLATILE", DISK_RESULT, 0);
            return;
        }
        uint16_t* words = memory.read16Array(regionKey(region), static_cast<size_t>(addr),
                                              static_cast<size_t>(len));
        if (!words) {
            memWrite("VOLATILE", DISK_RESULT, 0);
            return;
        }
        bool ok = memory.diskWriteEntry(name, /*type=*/3, /*flags=*/0, words, static_cast<size_t>(len));
        delete[] words;
        memWrite("VOLATILE", DISK_RESULT, static_cast<uint16_t>(ok ? 1 : 0));
    }
    else if (cmd == DISK_CMD_DELETE) {
        std::string name = readDiskNamePtr();
        bool ok = memory.diskDeleteEntry(name);
        memWrite("VOLATILE", DISK_RESULT, static_cast<uint16_t>(ok ? 1 : 0));
    }
    else if (cmd == DISK_CMD_EXEC) {
        std::string name = readDiskNamePtr();
        DiskEntry entry;
        std::vector<uint16_t> words;
        if (!memory.diskFindEntry(name, entry) || !memory.diskReadEntryWords(entry, words)) {
            memWrite("VOLATILE", DISK_RESULT, 0xFFFF);
            return;
        }
        // One-way chain-load: overwrite the running program and jump to
        // its start. VOLATILE is left untouched (registers are the CPU's
        // own state, not VOLATILE memory, so resetting them below doesn't
        // affect it) so a launcher can pass data through it by convention.
        installProgram(words);
        registers.fill(0);
        sp = STACK_TOP;
        carry = false;
        // Result is only observable if the loaded program reads it back
        // itself before doing anything else, but set it for completeness.
        memWrite("VOLATILE", DISK_RESULT, static_cast<uint16_t>(words.size() & 0xFFFF));
    }
}

bool NACPU::bootFromDisk() {
    DiskEntry entry;
    if (!memory.diskFindBootable(entry)) {
        std::cerr << "NACPU: disk has no bootable entry\n";
        return false;
    }
    std::vector<uint16_t> words;
    if (!memory.diskReadEntryWords(entry, words)) {
        std::cerr << "NACPU: failed to read boot entry '" << entry.name << "'\n";
        return false;
    }
    installProgram(words);
    registers.fill(0);
    sp = STACK_TOP;
    carry = false;
    running = true;
    std::cout << "NACPU: booted '" << entry.name << "' (" << words.size()
              << " words) from disk\n";
    return true;
}

void NACPU::updateHardwareFlags() {
    // November::update() already polls SDL events and sets shouldExit.
    // Mirror the Python hardware-flag ports so guest code can read them.
    int display_active = display.shouldQuit() ? 0 : 1;
    int halt_requested = display.shouldQuit() ? 1 : 0;

    memWrite("VOLATILE", 0xFD, static_cast<uint16_t>(host_frame & 0xFFFF));
    memWrite("VOLATILE", 0xFF, static_cast<uint16_t>(display_active));
    memWrite("VOLATILE", 0xFE, static_cast<uint16_t>(halt_requested));

    if (!display_active) {
        running = false;
    }

    pollTimer();
    pollInputEdge();
}

void NACPU::signalVblank() {
    raiseIrq(IRQ_VBLANK);
}

// ---------------------------------------------------------------------------
// Program loading
// ---------------------------------------------------------------------------
void NACPU::loadProgram(const std::vector<std::vector<std::string>>& program) {
    size_t address = 0;
    for (const auto& ins : program) {
        if (ins.empty()) continue;
        const std::string& opcode_name = ins[0];
        auto it = std::find(OPCODES.begin(), OPCODES.end(), opcode_name);
        if (it == OPCODES.end()) {
            throw std::runtime_error("Unknown opcode in program: " + opcode_name);
        }
        int opcode_word = static_cast<int>(std::distance(OPCODES.begin(), it));
        memWrite("NPROG", address, static_cast<uint16_t>(opcode_word));
        ++address;

        for (size_t i = 1; i < ins.size(); ++i) {
            int op = 0;
            try {
                // Accept decimal, 0x-hex, or plain hex
                op = std::stoi(ins[i], nullptr, 0);
            } catch (...) {
                op = 0;
            }
            memWrite("NPROG", address, static_cast<uint16_t>(wrap(op)));
            ++address;
        }
    }
    program_end = static_cast<int>(address);
    pc = 0;
    running = true;
}

void NACPU::loadProgram(const std::vector<Instruction>& program) {
    size_t address = 0;
    for (const auto& ins : program) {
        auto it = std::find(OPCODES.begin(), OPCODES.end(), ins.opcode_name);
        if (it == OPCODES.end()) {
            throw std::runtime_error("Unknown opcode in program: " + ins.opcode_name);
        }
        int opcode_word = static_cast<int>(std::distance(OPCODES.begin(), it));
        memWrite("NPROG", address, static_cast<uint16_t>(opcode_word));
        ++address;
        for (int op : ins.operands) {
            memWrite("NPROG", address, static_cast<uint16_t>(wrap(op)));
            ++address;
        }
    }
    program_end = static_cast<int>(address);
    pc = 0;
    running = true;
}

// ---------------------------------------------------------------------------
// .nu assembly support
// ---------------------------------------------------------------------------
bool NACPU::isRegisterToken(const std::string& token) {
    // Accept R0..R15, R00..R0F, r0..r15 (case-insensitive)
    if (token.size() < 2 || (token[0] != 'R' && token[0] != 'r')) return false;
    for (size_t i = 1; i < token.size(); ++i) {
        if (!std::isxdigit(static_cast<unsigned char>(token[i]))) return false;
    }
    return true;
}

int NACPU::parseOperand(const std::string& token) {
    if (token.empty()) return 0;

    // Register: R0 / R00 / R0B / R15 -> register index 0-15
    if (isRegisterToken(token)) {
        int reg = 0;
        try {
            reg = std::stoi(token.substr(1), nullptr, 16);
        } catch (...) {
            reg = 0;
        }
        if (reg < 0 || reg > 15) {
            throw std::runtime_error("Register out of range (0-15): " + token);
        }
        return reg;
    }

    // Immediate / address: default base 16 (matches compile.py .nu output)
    // Also accept 0x / 0X prefix and decimal if explicitly needed.
    try {
        if (token.size() > 2 && token[0] == '0' &&
            (token[1] == 'x' || token[1] == 'X')) {
            return std::stoi(token, nullptr, 16);
        }
        // Bare token: treat as hex (0549, FFFF, 00FD, ...)
        return std::stoi(token, nullptr, 16);
    } catch (...) {
        throw std::runtime_error("Cannot parse operand: " + token);
    }
}

std::vector<Instruction> NACPU::parseNu(const std::string& source) {
    std::vector<Instruction> program;
    std::istringstream stream(source);
    std::string line;
    int lineNo = 0;

    while (std::getline(stream, line)) {
        ++lineNo;

        // Strip ; comments
        auto commentPos = line.find(';');
        if (commentPos != std::string::npos) {
            line = line.substr(0, commentPos);
        }

        // Trim whitespace
        auto notSpace = [](unsigned char c) { return !std::isspace(c); };
        line.erase(line.begin(), std::find_if(line.begin(), line.end(), notSpace));
        line.erase(std::find_if(line.rbegin(), line.rend(), notSpace).base(), line.end());
        if (line.empty()) continue;

        // Tokenize on whitespace
        std::istringstream ls(line);
        std::string opcode;
        ls >> opcode;
        if (opcode.empty()) continue;

        // Upper-case opcode for lookup
        std::string opUpper = opcode;
        for (char& c : opUpper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

        auto opIt = OPERAND_COUNTS.find(opUpper);
        if (opIt == OPERAND_COUNTS.end()) {
            throw std::runtime_error(
                "Unknown opcode '" + opcode + "' at line " + std::to_string(lineNo));
        }

        Instruction ins;
        ins.opcode_name = opUpper;

        std::string tok;
        while (ls >> tok) {
            // Upper-case register names for consistent parsing
            if (!tok.empty() && (tok[0] == 'r' || tok[0] == 'R')) {
                for (char& c : tok) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            try {
                ins.operands.push_back(parseOperand(tok));
            } catch (const std::exception& e) {
                throw std::runtime_error(
                    std::string(e.what()) + " at line " + std::to_string(lineNo));
            }
        }

        // Soft check: warn (via exception in strict mode) if operand count mismatches.
        // Some tools emit extra tokens; we accept exactly the expected count and
        // ignore trailing junk, but require at least the expected number.
        int expected = opIt->second;
        if (static_cast<int>(ins.operands.size()) < expected) {
            throw std::runtime_error(
                "Opcode " + opUpper + " expects " + std::to_string(expected) +
                " operand(s), got " + std::to_string(ins.operands.size()) +
                " at line " + std::to_string(lineNo));
        }
        if (static_cast<int>(ins.operands.size()) > expected) {
            ins.operands.resize(static_cast<size_t>(expected));
        }

        program.push_back(std::move(ins));
    }

    return program;
}

bool NACPU::loadNuFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        std::cerr << "NACPU: cannot open .nu file: " << path << '\n';
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    try {
        auto program = parseNu(ss.str());
        loadProgram(program);
        std::cout << "NACPU: loaded " << program.size()
                  << " instructions from " << path
                  << " (NPROG size " << program_end << " words)\n";
        return true;
    } catch (const std::exception& e) {
        std::cerr << "NACPU: failed to parse " << path << ": " << e.what() << '\n';
        return false;
    }
}

// ---------------------------------------------------------------------------
// Fetch / run
// ---------------------------------------------------------------------------
Instruction NACPU::fetch() {
    Instruction result;
    if (pc >= program_end || !running) {
        running = false;
        return result; // empty opcode_name signals end
    }

    int opcode_word = memRead("NPROG", static_cast<size_t>(pc));
    if (opcode_word < 0 || opcode_word >= static_cast<int>(OPCODES.size())) {
        running = false;
        return result;
    }
    result.opcode_name = std::string(OPCODES[static_cast<size_t>(opcode_word)]);

    auto cntIt = OPERAND_COUNTS.find(result.opcode_name);
    int count = (cntIt != OPERAND_COUNTS.end()) ? cntIt->second : 0;

    result.operands.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        result.operands.push_back(memRead("NPROG", static_cast<size_t>(pc + 1 + i)));
    }
    pc += 1 + count;
    return result;
}

void NACPU::run() {
    while (running) {
        Instruction instruction = fetch();
        operate(instruction);
        checkInterrupts();
    }
}

// ---------------------------------------------------------------------------
// Instruction execution (direct port of the Python operate method)
// ---------------------------------------------------------------------------
void NACPU::operate(const Instruction& instruction) {
    if (instruction.opcode_name.empty()) return;

    const std::string& opcode = instruction.opcode_name;
    auto op = [&](size_t i) -> int {
        return (i < instruction.operands.size()) ? instruction.operands[i] : 0;
    };

    if (opcode == "NOP") {
        // nothing
    }
    else if (opcode == "MOV") {
        int src_reg = op(0);
        int dest_addr = op(1);
        if (dest_addr == IRQ_PENDING) {
            // Writing to IRQ_PENDING clears the bits that are set in the register
            uint16_t pending = memRead("VOLATILE", IRQ_PENDING);
            pending &= static_cast<uint16_t>(~wrap(static_cast<int>(getReg(src_reg))));
            memWrite("VOLATILE", IRQ_PENDING, static_cast<uint16_t>(wrap(pending)));
        } else {
            memWrite("VOLATILE", static_cast<size_t>(dest_addr),
                     static_cast<uint16_t>(wrap(static_cast<int>(getReg(src_reg)))));
            if (dest_addr == GPU_CMD) {
                gpuDispatch();
            } else if (dest_addr == DISK_CMD) {
                diskDispatch();
            }
        }
    }
    else if (opcode == "SMV") {
        int src_reg = op(0);
        int dest_addr = op(1);
        memWrite("DISK", static_cast<size_t>(dest_addr),
                 static_cast<uint16_t>(wrap(static_cast<int>(getReg(src_reg)))));
    }
    else if (opcode == "VMV") {
        int src_reg = op(0);
        int dest_addr = op(1);
        uint16_t val = static_cast<uint16_t>(wrap(static_cast<int>(getReg(src_reg))));
        if (dest_addr == 0xFFFF) {
            display.clear(val);
        } else {
            memWrite("VRAM", static_cast<size_t>(dest_addr), val);
        }
    }
    else if (opcode == "LDM") {
        int dest = op(0);
        int addr = op(1);
        set(dest, memRead("VOLATILE", static_cast<size_t>(addr)));
    }
    else if (opcode == "LOD") {
        int reg = op(0);
        int val = op(1);
        set(reg, val);
    }
    else if (opcode == "SWP") {
        int r1 = op(0);
        int r2 = op(1);
        int64_t a = getReg(r1);
        int64_t b = getReg(r2);
        set(r1, b);
        set(r2, a);
    }
    else if (opcode == "ADD") {
        int dest = op(0), src1 = op(1), src2 = op(2);
        int64_t total = static_cast<int64_t>(getReg(src1)) + static_cast<int64_t>(getReg(src2));
        carry = total > 0xFFFF;
        set(dest, total);
    }
    else if (opcode == "ADC") {
        int dest = op(0), src1 = op(1), src2 = op(2);
        int64_t total = static_cast<int64_t>(getReg(src1)) + static_cast<int64_t>(getReg(src2))
                        + (carry ? 1 : 0);
        carry = total > 0xFFFF;
        set(dest, total);
    }
    else if (opcode == "SUB") {
        int dest = op(0), src1 = op(1), src2 = op(2);
        int64_t diff = static_cast<int64_t>(getReg(src1)) - static_cast<int64_t>(getReg(src2));
        carry = diff < 0;
        set(dest, diff);
    }
    else if (opcode == "SBB") {
        int dest = op(0), src1 = op(1), src2 = op(2);
        int64_t diff = static_cast<int64_t>(getReg(src1)) - static_cast<int64_t>(getReg(src2))
                       - (carry ? 1 : 0);
        carry = diff < 0;
        set(dest, diff);
    }
    else if (opcode == "INC") {
        int reg = op(0);
        set(reg, static_cast<int64_t>(getReg(reg)) + 1);
    }
    else if (opcode == "DEC") {
        int reg = op(0);
        set(reg, static_cast<int64_t>(getReg(reg)) - 1);
    }
    else if (opcode == "MUL") {
        int dest = op(0), src1 = op(1), src2 = op(2);
        set(dest, static_cast<int64_t>(getReg(src1)) * static_cast<int64_t>(getReg(src2)));
    }
    else if (opcode == "DIV") {
        int dest = op(0), src1 = op(1), src2 = op(2);
        if (getReg(src2) == 0) {
            set(dest, 0);
        } else {
            set(dest, static_cast<int64_t>(getReg(src1)) / static_cast<int64_t>(getReg(src2)));
        }
    }
    else if (opcode == "AND") {
        int dest = op(0), src1 = op(1), src2 = op(2);
        set(dest, static_cast<int64_t>(getReg(src1)) & static_cast<int64_t>(getReg(src2)));
    }
    else if (opcode == "OR") {
        int dest = op(0), src1 = op(1), src2 = op(2);
        set(dest, static_cast<int64_t>(getReg(src1)) | static_cast<int64_t>(getReg(src2)));
    }
    else if (opcode == "XOR") {
        int dest = op(0), src1 = op(1), src2 = op(2);
        set(dest, static_cast<int64_t>(getReg(src1)) ^ static_cast<int64_t>(getReg(src2)));
    }
    else if (opcode == "NOT") {
        int dest = op(0), src = op(1);
        set(dest, ~static_cast<int64_t>(getReg(src)));
    }
    else if (opcode == "NEG") {
        int dest = op(0), src = op(1);
        set(dest, -static_cast<int64_t>(getReg(src)));
    }
    else if (opcode == "SHL" || opcode == "SAL") {
        int dest = op(0), src = op(1), amount = op(2);
        set(dest, static_cast<int64_t>(getReg(src)) << amount);
    }
    else if (opcode == "SHR") {
        int dest = op(0), src = op(1), amount = op(2);
        set(dest, wrap(static_cast<int>(getReg(src))) >> amount);
    }
    else if (opcode == "SAR") {
        int dest = op(0), src = op(1), amount = op(2);
        set(dest, signed16(static_cast<int>(getReg(src))) >> amount);
    }
    else if (opcode == "JMP") {
        pc = op(0);
    }
    else if (opcode == "JE") {
        int r1 = op(0), r2 = op(1), target = op(2);
        if (getReg(r1) == getReg(r2)) pc = target;
    }
    else if (opcode == "JNE") {
        int r1 = op(0), r2 = op(1), target = op(2);
        if (getReg(r1) != getReg(r2)) pc = target;
    }
    else if (opcode == "JZ") {
        int reg = op(0), target = op(1);
        if (getReg(reg) == 0) pc = target;
    }
    else if (opcode == "JNZ") {
        int reg = op(0), target = op(1);
        if (getReg(reg) != 0) pc = target;
    }
    else if (opcode == "JL") {
        int r1 = op(0), r2 = op(1), target = op(2);
        if (signed16(static_cast<int>(getReg(r1))) < signed16(static_cast<int>(getReg(r2))))
            pc = target;
    }
    else if (opcode == "JLE") {
        int r1 = op(0), r2 = op(1), target = op(2);
        if (signed16(static_cast<int>(getReg(r1))) <= signed16(static_cast<int>(getReg(r2))))
            pc = target;
    }
    else if (opcode == "JG") {
        int r1 = op(0), r2 = op(1), target = op(2);
        if (signed16(static_cast<int>(getReg(r1))) > signed16(static_cast<int>(getReg(r2))))
            pc = target;
    }
    else if (opcode == "JGE") {
        int r1 = op(0), r2 = op(1), target = op(2);
        if (signed16(static_cast<int>(getReg(r1))) >= signed16(static_cast<int>(getReg(r2))))
            pc = target;
    }
    else if (opcode == "JB") {
        int r1 = op(0), r2 = op(1), target = op(2);
        if (wrap(static_cast<int>(getReg(r1))) < wrap(static_cast<int>(getReg(r2))))
            pc = target;
    }
    else if (opcode == "JBE") {
        int r1 = op(0), r2 = op(1), target = op(2);
        if (wrap(static_cast<int>(getReg(r1))) <= wrap(static_cast<int>(getReg(r2))))
            pc = target;
    }
    else if (opcode == "JA") {
        int r1 = op(0), r2 = op(1), target = op(2);
        if (wrap(static_cast<int>(getReg(r1))) > wrap(static_cast<int>(getReg(r2))))
            pc = target;
    }
    else if (opcode == "JAE") {
        int r1 = op(0), r2 = op(1), target = op(2);
        if (wrap(static_cast<int>(getReg(r1))) >= wrap(static_cast<int>(getReg(r2))))
            pc = target;
    }
    else if (opcode == "CAL") {
        push(pc);
        pc = op(0);
    }
    else if (opcode == "RET") {
        pc = pop();
    }
    else if (opcode == "FOR") {
        int counter_reg = op(0);
        int limit = op(1);
        int target = op(2);
        set(counter_reg, static_cast<int64_t>(getReg(counter_reg)) + 1);
        if (static_cast<int>(getReg(counter_reg)) < limit) {
            pc = target;
        }
    }
    else if (opcode == "BRK") {
        running = false;
    }
    else if (opcode == "FLP") {
        // Present the frame (vblank) and raise the IRQ
        display.update();
        ++host_frame;
        signalVblank();
    }
    else if (opcode == "LSM") {
        int dest = op(0), addr = op(1);
        set(dest, memRead("DISK", static_cast<size_t>(addr)));
    }
    else if (opcode == "LVM") {
        int dest = op(0), addr = op(1);
        set(dest, memRead("VRAM", static_cast<size_t>(addr)));
    }
    else if (opcode == "LNP") {
        int dest = op(0), addr = op(1);
        set(dest, memRead("NPROG", static_cast<size_t>(addr)));
    }
    else if (opcode == "SNP") {
        int src_reg = op(0), dest_addr = op(1);
        memWrite("NPROG", static_cast<size_t>(dest_addr),
                 static_cast<uint16_t>(wrap(static_cast<int>(getReg(src_reg)))));
    }
    // ----- register-indirect forms -----
    else if (opcode == "MOVI") {
        int src_reg = op(0);
        int addr_reg = op(1);
        int dest_addr = wrap(static_cast<int>(getReg(addr_reg)));
        if (dest_addr == IRQ_PENDING) {
            uint16_t pending = memRead("VOLATILE", IRQ_PENDING);
            pending &= static_cast<uint16_t>(~wrap(static_cast<int>(getReg(src_reg))));
            memWrite("VOLATILE", IRQ_PENDING, static_cast<uint16_t>(wrap(pending)));
        } else {
            memWrite("VOLATILE", static_cast<size_t>(dest_addr),
                     static_cast<uint16_t>(wrap(static_cast<int>(getReg(src_reg)))));
            if (dest_addr == GPU_CMD) {
                gpuDispatch();
            } else if (dest_addr == DISK_CMD) {
                diskDispatch();
            }
        }
    }
    else if (opcode == "SMVI") {
        int src_reg = op(0), addr_reg = op(1);
        memWrite("DISK", static_cast<size_t>(wrap(static_cast<int>(getReg(addr_reg)))),
                 static_cast<uint16_t>(wrap(static_cast<int>(getReg(src_reg)))));
    }
    else if (opcode == "VMVI") {
        int src_reg = op(0), addr_reg = op(1);
        uint16_t val = static_cast<uint16_t>(wrap(static_cast<int>(getReg(src_reg))));
        int dest_addr = wrap(static_cast<int>(getReg(addr_reg)));
        if (dest_addr == 0xFFFF) {
            display.clear(val);
        } else {
            memWrite("VRAM", static_cast<size_t>(dest_addr), val);
        }
    }
    else if (opcode == "LDMI") {
        int dest = op(0), addr_reg = op(1);
        set(dest, memRead("VOLATILE", static_cast<size_t>(wrap(static_cast<int>(getReg(addr_reg))))));
    }
    else if (opcode == "LSMI") {
        int dest = op(0), addr_reg = op(1);
        set(dest, memRead("DISK", static_cast<size_t>(wrap(static_cast<int>(getReg(addr_reg))))));
    }
    else if (opcode == "LVMI") {
        int dest = op(0), addr_reg = op(1);
        set(dest, memRead("VRAM", static_cast<size_t>(wrap(static_cast<int>(getReg(addr_reg))))));
    }
    else if (opcode == "LNPI") {
        int dest = op(0), addr_reg = op(1);
        set(dest, memRead("NPROG", static_cast<size_t>(wrap(static_cast<int>(getReg(addr_reg))))));
    }
    else if (opcode == "SNPI") {
        int src_reg = op(0), addr_reg = op(1);
        memWrite("NPROG", static_cast<size_t>(wrap(static_cast<int>(getReg(addr_reg)))),
                 static_cast<uint16_t>(wrap(static_cast<int>(getReg(src_reg)))));
    }
    else if (opcode == "LBM") {
        int dest = op(0), addr = op(1);
        set(dest, memRead("SYSROM", static_cast<size_t>(addr)));
    }
    else if (opcode == "LBMI") {
        int dest = op(0), addr_reg = op(1);
        set(dest, memRead("SYSROM", static_cast<size_t>(wrap(static_cast<int>(getReg(addr_reg))))));
    }
    // ----- stack / interrupt control -----
    else if (opcode == "PSH") {
        push(getReg(op(0)));
    }
    else if (opcode == "POP") {
        set(op(0), pop());
    }
    else if (opcode == "LSP") {
        sp = wrap(static_cast<int>(getReg(op(0))));
    }
    else if (opcode == "SSP") {
        set(op(0), sp);
    }
    else if (opcode == "SEI") {
        ie = true;
        updateIrqStatusMirror();
    }
    else if (opcode == "CLI") {
        ie = false;
        updateIrqStatusMirror();
    }
    else if (opcode == "RTI") {
        int status = pop();
        pc = pop();
        unpackStatus(status);
        in_isr = false;
        updateIrqStatusMirror();
    }
    else if (opcode == "SWI") {
        raiseIrq(IRQ_SOFT);
    }
    else if (opcode == "PRT") {
        int reg = op(0);
        std::cout << "Register " << reg << " = " << getReg(reg)
                  << " (0x" << std::hex << wrap(static_cast<int>(getReg(reg)))
                  << std::dec << ")\n";
    }
    else {
        throw std::runtime_error("Unknown opcode: " + opcode);
    }
}
