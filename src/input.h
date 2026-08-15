//
// Created by rwarr on 8/8/2026.
//

#ifndef NUEVOAUTO_INPUT_H
#define NUEVOAUTO_INPUT_H

#define KEY_SLOTS 0x000F   // $8000-$800E used, $800F left as terminator/reserved

#include <deque>
#include <cstdint>

#include "memory.h"
#include "november.h"

class Debugger; // forward-declared: only input.cpp needs the full type

// Mirrors python_ref/input.py IO. Owns the SDL event pump for the whole
// app: call captureInput() exactly once per host frame, before running
// any CPU instructions. Nothing else should call SDL_PollEvent, or
// events will get split between pollers and dropped.
class Input {
public:
    Input(Memory& mem, November& nov);

    std::deque<uint16_t> key_queue;   // ASCII/unicode codes waiting to be presented, in typed order
    uint16_t current_ascii = 0;       // Value currently sitting in $8000
    bool presenting = false;          // True while current_ascii is the nonzero "half" of a cycle

    void captureInput();

    // Wire up the debug overlay so its ImGui backend sees every SDL event
    // and so keystrokes/clicks it wants get held back from the emulator.
    void setDebugger(Debugger* dbg) { debugger = dbg; }

private:
    Memory& mem;
    November& display;
    Debugger* debugger = nullptr;
};

#endif //NUEVOAUTO_INPUT_H
