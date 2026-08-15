//
// Created by rwarr on 8/8/2026.
//
// Dear ImGui port of python_ref/debugger.py. Runs as an overlay panel on
// November's own window/renderer (toggle: F1) instead of a separate Tk
// window/thread -- see November::setDebugOverlay(). Reuses NACPU.h's
// OPCODES / OPERAND_COUNTS tables directly instead of duplicating them
// the way debugger.py had to (it couldn't rely on cpu.py being importable
// from its own thread).
//

#ifndef NUEVOAUTO_DEBUGGER_H
#define NUEVOAUTO_DEBUGGER_H

#include "memory.h"
#include "NACPU.h"
#include "november.h"

#include <SDL3/SDL.h>
#include <string>
#include <vector>
#include <map>
#include <utility>

class Debugger {
public:
    Debugger(Memory& mem, NACPU& cpu, November& nov);
    ~Debugger();

    Debugger(const Debugger&) = delete;
    Debugger& operator=(const Debugger&) = delete;

    // Forward every SDL event here (from Input::captureInput()) so ImGui
    // sees mouse/keyboard/text input while the overlay is open.
    void processEvent(const SDL_Event& event);

    void toggle();
    bool isVisible() const { return visible; }

    // True once ImGui has claimed the keyboard/mouse this frame (e.g. the
    // person is typing into a hex field) -- Input should not also feed
    // that key into the emulated key_queue while this is true.
    bool wantsKeyboard() const;
    bool wantsMouse() const;

private:
    Memory& memory;
    NACPU& cpu;
    November& display;
    bool visible = false;

    // Region + view window
    std::string region = "nprog";     // default so PC is visible, matches debugger.py
    int offset = 0;
    char offsetBuf[8] = "0000";

    // Write controls
    char writeAddrBuf[8] = "";
    char writeValBuf[8] = "";
    std::string statusMsg;

    // Change-highlight bookkeeping (mirrors debugger.py's _recent_writes)
    std::string prevRegion;
    int prevOffset = -1;
    std::vector<uint16_t> prevData;
    std::map<std::pair<std::string, int>, double> recentWrites; // (region,addr) -> expire time
    static constexpr double WRITE_TTL = 1.5; // seconds

    void render(); // installed as November's debug overlay callback
    void writeMemory();
    std::string decodeInstruction(int pc) const;
    std::string regsSummary() const;
};

#endif //NUEVOAUTO_DEBUGGER_H
