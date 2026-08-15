//
// Created by rwarr on 8/6/2026.
//

#ifndef NUEVOAUTO_DISPLAY_H
#define NUEVOAUTO_DISPLAY_H

#include "memory.h"
#include <SDL3/SDL.h>
#include <functional>

#pragma once

#define NATIVE_WIDTH 320
#define NATIVE_HEIGHT 240
#define NUM_PLANES 6
#define PLANE_WORDS_PER_ROW (NATIVE_WIDTH / 16)   // 20 -- 16px packed per word
#define PLANE_SIZE (NATIVE_HEIGHT * PLANE_WORDS_PER_ROW)

#define BITPLANE_BASE 0
#define BITPLANE_TOTAL (NUM_PLANES * PLANE_SIZE)          // 28800

#define SPRITE_COUNT 16
#define SPRITE_SIZE 32
#define SPRITE_WORDS_PER_ROW (SPRITE_SIZE / 4) // 4bpp -> 4px/word -> 8
#define SPRITE_PIXEL_WORDS (SPRITE_SIZE * SPRITE_WORDS_PER_ROW)  // 256 words/sprite

#define SPRITE_PIXELS_BASE (BITPLANE_BASE + BITPLANE_TOTAL)       // 28800
#define SPRITE_PIXELS_TOTAL (SPRITE_COUNT * SPRITE_PIXEL_WORDS)    // 4096

/*
Per-sprite attribute record (8 words):
    0 X, 1 Y (signed, screen coords, off-screen allowed for scroll-on)
    2 flags: bit0 enabled, bit1 priority(behind bg), bit2 flip_h, bit3 flip_v
    3 scale_x (Q8.8 fixed point, 256 1.0x)
    4 scale_y (Q8.8 fixed point)
    5 rotation (degrees, 0-359)
    6 skew (signed degrees, shear angle)
    7 reserved
*/
#define SPRITE_ATTR_WORDS 8
#define SPRITE_ATTRS_BASE (SPRITE_PIXELS_BASE + SPRITE_PIXELS_TOTAL)   // 32896
#define SPRITE_ATTRS_TOTAL (SPRITE_COUNT * SPRITE_ATTR_WORDS)            // 128

// Each sprite has its own 16-color palette: master-palette indices (0-4095)
#define SPRITE_PAL_BASE (SPRITE_ATTRS_BASE + SPRITE_ATTRS_TOTAL)          // 33024
#define SPRITE_PAL_TOTAL (SPRITE_COUNT * 16)                              // 256

// 4096-color master palette: packed 12-bit RGB, one word each
//   bits 11:8 R, 7:4 G, 3:0 B  (each nibble 0-15, scaled by 17 -> 0-255)
#define MASTER_PALETTE_BASE (SPRITE_PAL_BASE + SPRITE_PAL_TOTAL)           // 33280
#define MASTER_PALETTE_TOTAL 4096

// The 64 on-screen background colors are indices into the master palette
#define ACTIVE_PALETTE_BASE (MASTER_PALETTE_BASE + MASTER_PALETTE_TOTAL)   // 37376
#define ACTIVE_PALETTE_TOTAL 64

// Split-screen / scroll registers
#define SPLIT_ENABLE (ACTIVE_PALETTE_BASE + ACTIVE_PALETTE_TOTAL)   // 37440
#define SPLIT_AXIS   (SPLIT_ENABLE + 1)   // 0 horizontal split (top/bottom), 1 vertical (left/right)
#define SPLIT_POS    (SPLIT_ENABLE + 2)   // row (0-239) or column (0-319) where the split happens
#define HSCROLL_A    (SPLIT_ENABLE + 3)   // shift for zone A (top/left), signed, wraps
#define HSCROLL_B    (SPLIT_ENABLE + 4)   // shift for zone B (bottom/right), signed, wraps

#define REGISTER_MAP_END (HSCROLL_B + 1)  // 37445


class November {
public:
    explicit November(Memory& mem);
    ~November();

    int init();
    void loadColors();
    void setMasterColor(int index, int r, int g, int b);
    std::vector<int> getMasterColor(int index);
    void setActivePalette(int slot, int index);
    std::vector<uint8_t> compose();
    std::vector<uint8_t> buffer();

    void drawPixel(int x, int y, int color);
    int getPixel(int x, int y);
    void drawLine(int x0, int y0, int x1, int y1, int color);
    void blitBuffer(std::vector<uint8_t> array);
    static int getWidth();
    static int getHeight();
    static int getPalLen();

    void clear(int color);

    void setSpritePixel(int id, int x, int y, int color);
    int getSpritePixel(int id, int x, int y);
    void setSpriteColor(int id, int pal_index, int master_idx);
    void configureSprite(
        int id,
        int x, int y,
        bool enabled,
        int priority,
        bool flip_h, bool flip_v,
        int scale_x, int scale_y,
        int rotation, int skew
        );
    void setSplit(bool enabled, bool axis, int pos);
    void setHScroll(int zone_a, int zone_b);
    void update();
    int getFPS();
    bool shouldQuit() const;
    void requestQuit();

    // Translate window pixel coords -> 320x240 logical screen coords
    // (accounts for the letterbox/scale from SDL_SetRenderLogicalPresentation).
    void windowToLogical(float windowX, float windowY, float& logicalX, float& logicalY) const;

    // Debugger hook: called once per update(), just before the frame is
    // presented, so overlay draws land on top of the composed emulator
    // frame without needing to touch November's render pipeline directly.
    SDL_Window* getWindow() const { return window; }
    SDL_Renderer* getRenderer() const { return renderer; }
    void setDebugOverlay(std::function<void()> cb) { debugOverlay = std::move(cb); }

private:
    Memory& memory;

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;

    Uint64 perfFreq = 0;
    Uint64 lastCounter = 0;
    double fps = 0.0;
    int frame = 1;
    int currentBackground = 0;
    bool shouldExit = false;

    // Cached active-palette RGB, rebuilt every update() from VRAM -
    // ACTIVE_PALETTE_TOTAL * 3 bytes (r,g,b per slot).
    std::vector<uint8_t> paletteRGB;

    std::function<void()> debugOverlay;

    void initDefaultRegisters() const;
    void loadWindowIcon() const;
    std::vector<int> readActivePalette();
    static int normalizeColor(int color);
    std::vector<int> spritePixels(int id);
    void renderSprites(std::vector<uint8_t>& rgb);
    void applyScrollSplit(std::vector<uint8_t>& rgb);
    static int16_t signed16(uint16_t v);
};

#endif //NUEVOAUTO_DISPLAY_H
