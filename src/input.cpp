//
// Created by rwarr on 8/8/2026.
//
// Port of python_ref/input.py. Differences from a naive translation:
//  - SDL3 has no per-event "unicode" field like pygame; printable text
//    comes from SDL_EVENT_TEXT_INPUT instead, so Enter/Backspace (which
//    don't generate text-input events) are handled separately via
//    SDL_EVENT_KEY_DOWN, same as the pygame elif chain.
//  - Continuous key state uses SDL_GetKeyboardState, which is indexed by
//    SDL_Scancode (physical key), not SDL_Keycode -- matches pygame's
//    key.get_pressed() which is also physical-key based.
//  - Mouse position is translated from window pixels to the emulator's
//    320x240 logical space via November::windowToLogical(), since SDL3's
//    logical presentation (letterbox) means window coords != screen coords.
//    pygame's SCALED flag did this translation for us automatically.
//

#include "input.h"
#include "debugger.h"

#include <SDL3/SDL.h>
#include <algorithm>

namespace {
    // Numpad keys are reserved for the joystick + digital buttons below.
    struct JoyKey { SDL_Scancode code; int dx; int dy; };
    constexpr JoyKey JOYSTICK_MAP[] = {
        {SDL_SCANCODE_KP_7, -1, -1}, {SDL_SCANCODE_KP_8, 0, -1}, {SDL_SCANCODE_KP_9, 1, -1},
        {SDL_SCANCODE_KP_4, -1,  0},                             {SDL_SCANCODE_KP_6, 1,  0},
        {SDL_SCANCODE_KP_1, -1,  1}, {SDL_SCANCODE_KP_2, 0,  1}, {SDL_SCANCODE_KP_3, 1,  1},
    };
    constexpr SDL_Scancode BUTTON_KEYS[4] = {
        SDL_SCANCODE_KP_0, SDL_SCANCODE_KP_DIVIDE, SDL_SCANCODE_KP_MULTIPLY, SDL_SCANCODE_KP_MINUS
    };

    // Decode the first UTF-8 codepoint of an SDL_EVENT_TEXT_INPUT string.
    // Good enough for our purposes: we only ever store one code point per
    // key_queue entry, same as ord(event.unicode) in the Python version.
    uint32_t firstCodepoint(const char* s) {
        if (!s || !*s) return 0;
        auto c0 = static_cast<unsigned char>(s[0]);
        uint32_t cp; int extra;
        if (c0 < 0x80)        { cp = c0;          extra = 0; }
        else if ((c0 & 0xE0) == 0xC0) { cp = c0 & 0x1F; extra = 1; }
        else if ((c0 & 0xF0) == 0xE0) { cp = c0 & 0x0F; extra = 2; }
        else if ((c0 & 0xF8) == 0xF0) { cp = c0 & 0x07; extra = 3; }
        else return 0;
        for (int i = 1; i <= extra && s[i]; ++i) {
            cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
        }
        return cp;
    }
}

Input::Input(Memory& mem, November& nov) : mem(mem), display(nov) {}

void Input::captureInput() {
    int scroll_down = 0, scroll_up = 0;

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (debugger) debugger->processEvent(event);

        switch (event.type) {
            case SDL_EVENT_QUIT:
                display.requestQuit();
                break;
            case SDL_EVENT_KEY_DOWN:
                if (event.key.key == SDLK_F1) {
                    if (debugger) debugger->toggle();
                } else if (event.key.key == SDLK_ESCAPE) {
                    display.requestQuit();
                } else if (debugger && debugger->wantsKeyboard()) {
                    // Overlay has the keyboard (typing into a hex field) --
                    // don't also feed this key to the emulated console.
                } else if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER) {
                    key_queue.push_back(13);
                } else if (event.key.key == SDLK_BACKSPACE) {
                    key_queue.push_back(8);
                }
                break;
            case SDL_EVENT_TEXT_INPUT: {
                if (debugger && debugger->wantsKeyboard()) break;
                uint32_t cp = firstCodepoint(event.text.text);
                if (cp != 0 && cp <= 0xFFFF) {
                    key_queue.push_back(static_cast<uint16_t>(cp));
                }
                break;
            }
            case SDL_EVENT_MOUSE_WHEEL:
                if (debugger && debugger->wantsMouse()) break;
                if (event.wheel.y < 0) scroll_down = 1;
                else if (event.wheel.y > 0) scroll_up = 1;
                break;
            default:
                break;
        }
    }

    // Present one queued key at a time: one frame nonzero, one frame zero.
    // Matches the BIOS debounce loop (waits for $8000 to read back 0
    // before treating the key as "released") while guaranteeing every
    // keystroke is eventually written.
    if (presenting) {
        current_ascii = 0;
        presenting = false;
    } else if (!key_queue.empty()) {
        current_ascii = key_queue.front();
        key_queue.pop_front();
        presenting = true;
    }

    uint16_t key_active[KEY_SLOTS] = {0};
    key_active[0] = current_ascii;
    mem.write16Array("volatile", 0x8000, key_active, KEY_SLOTS);

    // ------------------------------------------------------------------
    // Continuous state polling (Joystick, Mouse buttons)
    // ------------------------------------------------------------------
    const bool* pressed = SDL_GetKeyboardState(nullptr);

    // --- Joystick ($8010): high byte = X, low byte = Y, 0x7F = center ---
    int dx = 0, dy = 0;
    for (const auto& jk : JOYSTICK_MAP) {
        if (pressed[jk.code]) {
            dx += jk.dx;
            dy += jk.dy;
        }
    }
    dx = std::clamp(dx, -1, 1);
    dy = std::clamp(dy, -1, 1);
    uint16_t x_byte = static_cast<uint16_t>(0x7F + dx * 0x7F);
    uint16_t y_byte = static_cast<uint16_t>(0x7F + dy * 0x7F);
    mem.write16("volatile", 0x8010, static_cast<uint16_t>((x_byte << 8) | y_byte));

    // --- Digital buttons ($8011, $8012): one bit per numpad button ---
    uint16_t button_bits = 0;
    for (int i = 0; i < 4; ++i) {
        if (pressed[BUTTON_KEYS[i]]) button_bits |= static_cast<uint16_t>(1 << i);
    }
    mem.write16("volatile", 0x8011, button_bits);
    mem.write16("volatile", 0x8012, 0);

    // --- Mouse position ($8013, $8014), translated to 320x240 logical space ---
    bool mouseCaptured = debugger && debugger->wantsMouse();
    float wx = 0, wy = 0;
    Uint32 mouseState = SDL_GetMouseState(&wx, &wy);
    if (!mouseCaptured) {
        float lx = wx, ly = wy;
        display.windowToLogical(wx, wy, lx, ly);
        mem.write16("volatile", 0x8013, static_cast<uint16_t>(static_cast<int>(lx) & 0xFFFF));
        mem.write16("volatile", 0x8014, static_cast<uint16_t>(static_cast<int>(ly) & 0xFFFF));
    }

    // --- Mouse state ($8015): 0xWXYZ ---
    if (!mouseCaptured) {
        bool left_click = mouseState & SDL_BUTTON_MASK(SDL_BUTTON_LEFT);
        bool right_click = mouseState & SDL_BUTTON_MASK(SDL_BUTTON_RIGHT);
        uint16_t mouse_state = static_cast<uint16_t>(
            (scroll_down << 12) | (scroll_up << 8) | (left_click << 4) | right_click
        );
        mem.write16("volatile", 0x8015, mouse_state);
    }
}
