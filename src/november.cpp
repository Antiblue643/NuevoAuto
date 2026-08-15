//
// Created by rwarr on 8/6/2026.
//

#include "november.h"
#include <SDL3/SDL.h>
#include <fstream>
#include <cmath>
#include <algorithm>
#include <array>

#include "extern/stb_image.h"

/*
The NuevoAuto's "November" is an Amiga 500-like graphics chip.

Real chip, not a linear framebuffer:
- Background is 6 true bitplanes (packed 1-bit-per-pixel word arrays,
16 pixels/word), combined per-pixel into a 6-bit index -> 64 colors
on screen at once, out of a 4096-color (12-bit RGB, 4 bits/channel)
master palette. This is the classic Amiga OCS scheme.
- 16 hardware sprites, fixed 32x32 @ 4bpp (16 colors each, index 0
transparent), independently translatable/scalable/rotatable/skewed.
- One hardware split (horizontal or vertical) that lets the top/bottom
(or left/right) half of the screen scroll independently - a stand-in
for h-blank copper tricks.
- A per-zone horizontal scroll register, applied with array
roll/slicing rather than actually moving pixel data.

All of this state lives in VRAM as memory-mapped registers, exactly the
same way "write 0xFFFF to VRAM clears the screen" already worked: a
program pokes values into fixed VRAM addresses with VMV/VMVI and the
chip picks them up.
*/

namespace {
    constexpr double PI = 3.14159265358979323846;
    // Match Python november.py: self.clock.tick(30)
    constexpr int TARGET_FPS = 30;
}

November::November(Memory& mem) : memory(mem) {
    paletteRGB.assign(ACTIVE_PALETTE_TOTAL * 3, 0);
}

November::~November() {
    if (texture) SDL_DestroyTexture(texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
}

// ------------------------------------------------------------------
// Setup
// ------------------------------------------------------------------
int November::init() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("November: SDL_Init failed: %s", SDL_GetError());
        return -1;
    }

    window = SDL_CreateWindow("NuevoAuto 0x0010", NATIVE_WIDTH * 2, NATIVE_HEIGHT * 2, SDL_WINDOW_RESIZABLE);
    if (!window) {
        SDL_Log("November: SDL_CreateWindow failed: %s", SDL_GetError());
        return -1;
    }
    loadWindowIcon();

    renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        SDL_Log("November: SDL_CreateRenderer failed: %s", SDL_GetError());
        return -1;
    }
    SDL_SetRenderLogicalPresentation(renderer, NATIVE_WIDTH, NATIVE_HEIGHT, SDL_LOGICAL_PRESENTATION_LETTERBOX);

    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, NATIVE_WIDTH, NATIVE_HEIGHT);
    if (!texture) {
        SDL_Log("November: SDL_CreateTexture failed: %s", SDL_GetError());
        return -1;
    }
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
    SDL_HideCursor();
    SDL_StartTextInput(window); // needed for SDL_EVENT_TEXT_INPUT (see input.cpp)

    perfFreq = SDL_GetPerformanceFrequency();
    lastCounter = SDL_GetPerformanceCounter();

    loadColors();
    initDefaultRegisters();
    readActivePalette();

    return 0;
}

void November::initDefaultRegisters() const {
    memory.write16("vram", SPLIT_ENABLE, 0);
    memory.write16("vram", SPLIT_AXIS, 0);
    memory.write16("vram", SPLIT_POS, NATIVE_HEIGHT / 2);
    memory.write16("vram", HSCROLL_A, 0);
    memory.write16("vram", HSCROLL_B, 0);
    for (int s = 0; s < SPRITE_COUNT; ++s) {
        int base = SPRITE_ATTRS_BASE + s * SPRITE_ATTR_WORDS;
        memory.write16("vram", base + 2, 0);    // disabled
        memory.write16("vram", base + 3, 256);  // scale_x = 1.0
        memory.write16("vram", base + 4, 256);  // scale_y = 1.0
    }
}

void November::loadWindowIcon() const {
    // Same emures-not-disk reasoning as loadColors(): the window/exe icon
    // is chip/host-owned presentation, not a disk asset.
    int w = 0, h = 0, channels = 0;
    unsigned char* data = stbi_load("emures/icon.png", &w, &h, &channels, 4);
    if (!data) {
        SDL_Log("November: failed to load window icon 'emures/icon.png': %s", stbi_failure_reason());
        return;
    }

    SDL_Surface* icon = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, data, w * 4);
    if (!icon) {
        SDL_Log("November: failed to create icon surface: %s", SDL_GetError());
        stbi_image_free(data);
        return;
    }

    SDL_SetWindowIcon(window, icon);
    SDL_DestroySurface(icon);
    stbi_image_free(data);
}

void November::loadColors() {
    // Boot palette: one hex 0xRRGGBB color per line in emures/colors.ncp.
    // Lives in emures (not disk) because it's chip-owned default state,
    // not a disk asset - see the memory/disk rework notes.
    std::vector<uint32_t> colors;
    std::ifstream f("emures/colors.ncp");
    if (f.is_open()) {
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            try {
                colors.push_back(static_cast<uint32_t>(std::stoul(line, nullptr, 16)));
            } catch (...) {
                // skip malformed lines
            }
        }
    }

    if (colors.empty()) {
        SDL_Log("November: emures/colors.ncp not found - generating a default master palette.");
        for (int i = 0; i < MASTER_PALETTE_TOTAL; ++i) {
            int r = (i * 7) & 0xFF;
            int g = (i * 13) & 0xFF;
            int b = (i * 29) & 0xFF;
            colors.push_back((static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b));
        }
    }

    int n = std::min(static_cast<int>(colors.size()), MASTER_PALETTE_TOTAL);
    for (int i = 0; i < n; ++i) {
        uint32_t c = colors[i];
        setMasterColor(i, (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
    }

    int nActive = std::min(static_cast<int>(colors.size()), ACTIVE_PALETTE_TOTAL);
    for (int slot = 0; slot < nActive; ++slot) {
        memory.write16("vram", ACTIVE_PALETTE_BASE + slot, static_cast<uint16_t>(slot));
    }
    for (int slot = nActive; slot < ACTIVE_PALETTE_TOTAL; ++slot) {
        memory.write16("vram", ACTIVE_PALETTE_BASE + slot, 0);
    }
}

// ------------------------------------------------------------------
// Master palette (4096 colors, 12-bit RGB packed into a word)
// ------------------------------------------------------------------
void November::setMasterColor(int index, int r, int g, int b) {
    int idx = ((index % MASTER_PALETTE_TOTAL) + MASTER_PALETTE_TOTAL) % MASTER_PALETTE_TOTAL;
    uint16_t word = static_cast<uint16_t>((((r >> 4) & 0xF) << 8) | (((g >> 4) & 0xF) << 4) | ((b >> 4) & 0xF));
    memory.write16("vram", MASTER_PALETTE_BASE + idx, word);
}

std::vector<int> November::getMasterColor(int index) {
    int idx = ((index % MASTER_PALETTE_TOTAL) + MASTER_PALETTE_TOTAL) % MASTER_PALETTE_TOTAL;
    uint16_t word = memory.read16("vram", MASTER_PALETTE_BASE + idx);
    int r = ((word >> 8) & 0xF) * 17;
    int g = ((word >> 4) & 0xF) * 17;
    int b = (word & 0xF) * 17;
    return {r, g, b};
}

void November::setActivePalette(int slot, int index) {
    int s = ((slot % ACTIVE_PALETTE_TOTAL) + ACTIVE_PALETTE_TOTAL) % ACTIVE_PALETTE_TOTAL;
    int idx = ((index % MASTER_PALETTE_TOTAL) + MASTER_PALETTE_TOTAL) % MASTER_PALETTE_TOTAL;
    memory.write16("vram", ACTIVE_PALETTE_BASE + s, static_cast<uint16_t>(idx));
}

std::vector<int> November::readActivePalette() {
    std::vector<int> out(ACTIVE_PALETTE_TOTAL * 3);
    paletteRGB.assign(ACTIVE_PALETTE_TOTAL * 3, 0);
    for (int slot = 0; slot < ACTIVE_PALETTE_TOTAL; ++slot) {
        uint16_t masterIdx = memory.read16("vram", ACTIVE_PALETTE_BASE + slot);
        std::vector<int> rgb = getMasterColor(masterIdx);
        paletteRGB[slot * 3 + 0] = static_cast<uint8_t>(rgb[0]);
        paletteRGB[slot * 3 + 1] = static_cast<uint8_t>(rgb[1]);
        paletteRGB[slot * 3 + 2] = static_cast<uint8_t>(rgb[2]);
        out[slot * 3 + 0] = rgb[0];
        out[slot * 3 + 1] = rgb[1];
        out[slot * 3 + 2] = rgb[2];
    }
    return out;
}

int November::normalizeColor(int color) {
    // Wrap real colors into the 64-slot active palette; negative stays transparent.
    if (color >= 0) return color % ACTIVE_PALETTE_TOTAL;
    return color;
}

// ------------------------------------------------------------------
// Pixel-level drawing (loops per pixel - fine for small ops, use
// blitBuffer() for whole-screen effects)
// ------------------------------------------------------------------
void November::drawPixel(int x, int y, int color) {
    color = normalizeColor(color);
    if (color < 0) return; // transparent
    if (x < 0 || x >= NATIVE_WIDTH || y < 0 || y >= NATIVE_HEIGHT) return;

    int wordCol = x / 16;
    int bitInWord = 15 - (x % 16);
    for (int p = 0; p < NUM_PLANES; ++p) {
        int addr = BITPLANE_BASE + p * PLANE_SIZE + y * PLANE_WORDS_PER_ROW + wordCol;
        uint16_t word = memory.read16("vram", addr);
        if ((color >> p) & 1) word = static_cast<uint16_t>(word | (1 << bitInWord));
        else word = static_cast<uint16_t>(word & ~(1 << bitInWord));
        memory.write16("vram", addr, word);
    }
}

int November::getPixel(int x, int y) {
    if (x < 0 || x >= NATIVE_WIDTH || y < 0 || y >= NATIVE_HEIGHT) return 0;
    int wordCol = x / 16;
    int bitInWord = 15 - (x % 16);
    int index = 0;
    for (int p = 0; p < NUM_PLANES; ++p) {
        int addr = BITPLANE_BASE + p * PLANE_SIZE + y * PLANE_WORDS_PER_ROW + wordCol;
        uint16_t word = memory.read16("vram", addr);
        int bit = (word >> bitInWord) & 1;
        index |= bit << p;
    }
    return index;
}

void November::drawLine(int x0, int y0, int x1, int y1, int color) {
    int dx = std::abs(x1 - x0);
    int dy = -std::abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    int x = x0, y = y0;
    while (true) {
        drawPixel(x, y, color);
        if (x == x1 && y == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x += sx; }
        if (e2 <= dx) { err += dx; y += sy; }
    }
}

std::vector<uint8_t> November::compose() {
    // Decode all 6 bitplanes into one (H*W) chunky index (0-63) buffer.
    std::vector<uint8_t> chunky(static_cast<size_t>(NATIVE_HEIGHT) * NATIVE_WIDTH, 0);
    for (int p = 0; p < NUM_PLANES; ++p) {
        int base = BITPLANE_BASE + p * PLANE_SIZE;
        for (int y = 0; y < NATIVE_HEIGHT; ++y) {
            for (int wc = 0; wc < PLANE_WORDS_PER_ROW; ++wc) {
                uint16_t word = memory.read16("vram", base + y * PLANE_WORDS_PER_ROW + wc);
                for (int b = 0; b < 16; ++b) {
                    if ((word >> (15 - b)) & 1) {
                        int x = wc * 16 + b;
                        chunky[static_cast<size_t>(y) * NATIVE_WIDTH + x] |= static_cast<uint8_t>(1 << p);
                    }
                }
            }
        }
    }
    return chunky;
}

std::vector<uint8_t> November::buffer() {
    // Read-only chunky view for legacy callers. Use blitBuffer() to write.
    return compose();
}

void November::blitBuffer(std::vector<uint8_t> array) {
    size_t total = static_cast<size_t>(NATIVE_WIDTH) * NATIVE_HEIGHT;
    if (array.size() < total) array.resize(total, 0);
    for (auto& v : array) v = static_cast<uint8_t>(v % ACTIVE_PALETTE_TOTAL);

    for (int p = 0; p < NUM_PLANES; ++p) {
        int base = BITPLANE_BASE + p * PLANE_SIZE;
        for (int y = 0; y < NATIVE_HEIGHT; ++y) {
            for (int wc = 0; wc < PLANE_WORDS_PER_ROW; ++wc) {
                uint16_t word = 0;
                for (int b = 0; b < 16; ++b) {
                    int x = wc * 16 + b;
                    if ((array[static_cast<size_t>(y) * NATIVE_WIDTH + x] >> p) & 1) word = static_cast<uint16_t>(word | (1 << (15 - b)));
                }
                memory.write16("vram", base + y * PLANE_WORDS_PER_ROW + wc, word);
            }
        }
    }
}

int November::getWidth() { return NATIVE_WIDTH; }
int November::getHeight() { return NATIVE_HEIGHT; }
int November::getPalLen() { return ACTIVE_PALETTE_TOTAL; }

void November::clear(int color) {
    color = normalizeColor(color);
    currentBackground = color;
    for (int p = 0; p < NUM_PLANES; ++p) {
        uint16_t fill = ((color >> p) & 1) ? 0xFFFF : 0x0000;
        int base = BITPLANE_BASE + p * PLANE_SIZE;
        for (int i = 0; i < PLANE_SIZE; ++i) {
            memory.write16("vram", base + i, fill);
        }
    }
}

// ------------------------------------------------------------------
// Sprites - 16 hardware sprites, 32x32 @ 16 colors, transformable
// ------------------------------------------------------------------
void November::setSpritePixel(int id, int x, int y, int color) {
    // color is 0-15, palette-local to this sprite. 0 = transparent.
    if (x < 0 || x >= SPRITE_SIZE || y < 0 || y >= SPRITE_SIZE) return;
    int base = SPRITE_PIXELS_BASE + id * SPRITE_PIXEL_WORDS;
    int wordIdx = base + y * SPRITE_WORDS_PER_ROW + (x / 4);
    int nibbleShift = (3 - (x % 4)) * 4;
    uint16_t word = memory.read16("vram", wordIdx);
    word = static_cast<uint16_t>((word & ~(0xF << nibbleShift)) | ((color & 0xF) << nibbleShift));
    memory.write16("vram", wordIdx, word);
}

int November::getSpritePixel(int id, int x, int y) {
    if (x < 0 || x >= SPRITE_SIZE || y < 0 || y >= SPRITE_SIZE) return 0;
    int base = SPRITE_PIXELS_BASE + id * SPRITE_PIXEL_WORDS;
    int wordIdx = base + y * SPRITE_WORDS_PER_ROW + (x / 4);
    int nibbleShift = (3 - (x % 4)) * 4;
    return (memory.read16("vram", wordIdx) >> nibbleShift) & 0xF;
}

void November::setSpriteColor(int id, int pal_index, int master_idx) {
    int idx = ((pal_index % 16) + 16) % 16;
    int mIdx = ((master_idx % MASTER_PALETTE_TOTAL) + MASTER_PALETTE_TOTAL) % MASTER_PALETTE_TOTAL;
    memory.write16("vram", SPRITE_PAL_BASE + id * 16 + idx, static_cast<uint16_t>(mIdx));
}

void November::configureSprite(
    int id,
    int x, int y,
    bool enabled,
    int priority,
    bool flip_h, bool flip_v,
    int scale_x, int scale_y,
    int rotation, int skew
) {
    int base = SPRITE_ATTRS_BASE + id * SPRITE_ATTR_WORDS;
    memory.write16("vram", base + 0, static_cast<uint16_t>(x & 0xFFFF));
    memory.write16("vram", base + 1, static_cast<uint16_t>(y & 0xFFFF));

    uint16_t flags = 0;
    if (enabled) flags |= 1;
    if (priority) flags |= 2;
    if (flip_h) flags |= 4;
    if (flip_v) flags |= 8;
    memory.write16("vram", base + 2, flags);

    memory.write16("vram", base + 3, static_cast<uint16_t>(scale_x & 0xFFFF)); // Q8.8, 256 = 1.0x
    memory.write16("vram", base + 4, static_cast<uint16_t>(scale_y & 0xFFFF)); // Q8.8, 256 = 1.0x
    memory.write16("vram", base + 5, static_cast<uint16_t>(((rotation % 360) + 360) % 360));
    memory.write16("vram", base + 6, static_cast<uint16_t>(skew & 0xFFFF));
}

std::vector<int> November::spritePixels(int id) {
    std::vector<int> pixels(SPRITE_SIZE * SPRITE_SIZE, 0);
    int base = SPRITE_PIXELS_BASE + id * SPRITE_PIXEL_WORDS;
    for (int y = 0; y < SPRITE_SIZE; ++y) {
        for (int wc = 0; wc < SPRITE_WORDS_PER_ROW; ++wc) {
            uint16_t word = memory.read16("vram", base + y * SPRITE_WORDS_PER_ROW + wc);
            for (int n = 0; n < 4; ++n) {
                int shift = (3 - n) * 4;
                int x = wc * 4 + n;
                pixels[y * SPRITE_SIZE + x] = (word >> shift) & 0xF;
            }
        }
    }
    return pixels;
}

int16_t November::signed16(uint16_t v) {
    return static_cast<int16_t>(v);
}

// Composite all enabled sprites onto an (H*W*3) RGB frame, in place.
// Each sprite is anchored at its own center (x+16, y+16) and sampled
// via inverse transform (rotate -> scale -> skew -> flip) so scaling
// and rotation grow outward from that fixed center, matching the
// Python reference's pygame.transform pipeline in spirit. Note:
// the "priority" (behind bg) flag is stored but, same as the Python
// reference, not yet wired into the compositing order below.
void November::renderSprites(std::vector<uint8_t>& rgb) {
    for (int id = 0; id < SPRITE_COUNT; ++id) {
        int base = SPRITE_ATTRS_BASE + id * SPRITE_ATTR_WORDS;
        uint16_t flags = memory.read16("vram", base + 2);
        if (!(flags & 1)) continue; // disabled

        int sx = signed16(memory.read16("vram", base + 0));
        int sy = signed16(memory.read16("vram", base + 1));
        double scaleX = memory.read16("vram", base + 3) / 256.0;
        double scaleY = memory.read16("vram", base + 4) / 256.0;
        double rotation = memory.read16("vram", base + 5);
        double skew = signed16(memory.read16("vram", base + 6));
        bool flipH = flags & 4;
        bool flipV = flags & 8;

        if (scaleX <= 0.0) scaleX = 0.001;
        if (scaleY <= 0.0) scaleY = 0.001;

        std::vector<int> indices = spritePixels(id);
        std::array<std::array<uint8_t, 3>, 16> localPal{};
        for (int i = 0; i < 16; ++i) {
            uint16_t masterIdx = memory.read16("vram", SPRITE_PAL_BASE + id * 16 + i);
            std::vector<int> c = getMasterColor(masterIdx);
            localPal[i] = { static_cast<uint8_t>(c[0]), static_cast<uint8_t>(c[1]), static_cast<uint8_t>(c[2]) };
        }

        double rad = rotation * PI / 180.0;
        double icos = std::cos(rad), isin = std::sin(rad); // inverse (R(-rad)) coefficients
        double shear = std::tan(skew * PI / 180.0);

        int cx = sx + SPRITE_SIZE / 2;
        int cy = sy + SPRITE_SIZE / 2;
        double maxScale = std::max(scaleX, scaleY);
        double halfSpan = (SPRITE_SIZE * 0.5) * maxScale * 1.4142 + std::abs(shear) * SPRITE_SIZE * 0.5 + 1;
        int half = static_cast<int>(std::ceil(halfSpan));

        int x0 = std::max(0, cx - half), x1 = std::min(NATIVE_WIDTH, cx + half);
        int y0 = std::max(0, cy - half), y1 = std::min(NATIVE_HEIGHT, cy + half);

        for (int dy = y0; dy < y1; ++dy) {
            for (int dx = x0; dx < x1; ++dx) {
                double ddx = dx - cx + 0.5;
                double ddy = dy - cy + 0.5;

                // inverse rotate
                double p1x = icos * ddx + isin * ddy;
                double p1y = -isin * ddx + icos * ddy;

                // inverse scale
                double p2x = p1x / scaleX;
                double p2y = p1y / scaleY;

                // inverse skew (shear only offsets x, based on y)
                double vprime = p2y;
                double uprime = p2x - shear * vprime;

                // inverse flip
                if (flipH) uprime = -uprime;
                if (flipV) vprime = -vprime;

                int u = static_cast<int>(std::floor(uprime + SPRITE_SIZE / 2.0));
                int v = static_cast<int>(std::floor(vprime + SPRITE_SIZE / 2.0));
                if (u < 0 || u >= SPRITE_SIZE || v < 0 || v >= SPRITE_SIZE) continue;

                int idx = indices[v * SPRITE_SIZE + u];
                if (idx == 0) continue; // transparent

                size_t pi = (static_cast<size_t>(dy) * NATIVE_WIDTH + dx) * 3;
                rgb[pi + 0] = localPal[idx][0];
                rgb[pi + 1] = localPal[idx][1];
                rgb[pi + 2] = localPal[idx][2];
            }
        }
    }
}

// ------------------------------------------------------------------
// Split screen + horizontal scroll
// ------------------------------------------------------------------
void November::applyScrollSplit(std::vector<uint8_t>& rgb) {
    uint16_t splitEnabled = memory.read16("vram", SPLIT_ENABLE);
    int hscrollA = signed16(memory.read16("vram", HSCROLL_A));
    int hscrollB = signed16(memory.read16("vram", HSCROLL_B));

    auto rollRange = [&](int y, int startX, int width, int shift) {
        if (shift == 0 || width <= 0) return;
        int s = ((shift % width) + width) % width;
        std::vector<uint8_t> seg(rgb.begin() + (static_cast<size_t>(y) * NATIVE_WIDTH + startX) * 3,
                                  rgb.begin() + (static_cast<size_t>(y) * NATIVE_WIDTH + startX + width) * 3);
        for (int x = 0; x < width; ++x) {
            int srcX = ((x - s) % width + width) % width;
            size_t dst = (static_cast<size_t>(y) * NATIVE_WIDTH + startX + x) * 3;
            rgb[dst + 0] = seg[srcX * 3 + 0];
            rgb[dst + 1] = seg[srcX * 3 + 1];
            rgb[dst + 2] = seg[srcX * 3 + 2];
        }
    };

    if (!splitEnabled) {
        if (hscrollA) {
            for (int y = 0; y < NATIVE_HEIGHT; ++y) rollRange(y, 0, NATIVE_WIDTH, hscrollA);
        }
        return;
    }

    uint16_t axis = memory.read16("vram", SPLIT_AXIS);
    int pos = memory.read16("vram", SPLIT_POS);

    if (axis == 0) { // horizontal split: top/bottom
        pos = std::max(0, std::min(static_cast<int>(NATIVE_HEIGHT), pos));
        if (hscrollA) for (int y = 0; y < pos; ++y) rollRange(y, 0, NATIVE_WIDTH, hscrollA);
        if (hscrollB) for (int y = pos; y < NATIVE_HEIGHT; ++y) rollRange(y, 0, NATIVE_WIDTH, hscrollB);
    } else { // vertical split: left/right
        pos = std::max(0, std::min(static_cast<int>(NATIVE_WIDTH), pos));
        for (int y = 0; y < NATIVE_HEIGHT; ++y) {
            if (hscrollA) rollRange(y, 0, pos, hscrollA);
            if (hscrollB) rollRange(y, pos, NATIVE_WIDTH - pos, hscrollB);
        }
    }
}

void November::setSplit(bool enabled, bool axis, int pos) {
    memory.write16("vram", SPLIT_ENABLE, enabled ? 1 : 0);
    memory.write16("vram", SPLIT_AXIS, axis ? 1 : 0);
    memory.write16("vram", SPLIT_POS, static_cast<uint16_t>(pos & 0xFFFF));
}

void November::setHScroll(int zone_a, int zone_b) {
    memory.write16("vram", HSCROLL_A, static_cast<uint16_t>(zone_a & 0xFFFF));
    memory.write16("vram", HSCROLL_B, static_cast<uint16_t>(zone_b & 0xFFFF));
}

// ------------------------------------------------------------------
// Frame update
// ------------------------------------------------------------------
void November::update() {
    // Event polling lives in Input::captureInput() (called once per host
    // frame, before this), matching python_ref where november.py's
    // update() never touches pg.event.get() -- only input.py does.

    // Simple frame limiter targeting TARGET_FPS.
    constexpr double targetFrameTime = 1.0 / TARGET_FPS;
    Uint64 now = SDL_GetPerformanceCounter();
    double elapsed = static_cast<double>(now - lastCounter) / static_cast<double>(perfFreq);
    if (elapsed < targetFrameTime) {
        SDL_Delay(static_cast<Uint32>((targetFrameTime - elapsed) * 1000.0));
    }
    now = SDL_GetPerformanceCounter();
    double delta = static_cast<double>(now - lastCounter) / static_cast<double>(perfFreq);
    if (delta > 0.0) fps = 1.0 / delta;
    lastCounter = now;

    frame = (frame + 1) & 0xFFFF;

    // Registers are re-read every frame - a program can poke palette,
    // sprite, or split state at any point and it takes effect next
    // vblank, the same way real hardware would pick it up.
    readActivePalette();

    std::vector<uint8_t> chunky = compose();
    std::vector<uint8_t> rgb(static_cast<size_t>(NATIVE_WIDTH) * NATIVE_HEIGHT * 3);
    for (size_t i = 0; i < chunky.size(); ++i) {
        uint8_t idx = chunky[i];
        rgb[i * 3 + 0] = paletteRGB[idx * 3 + 0];
        rgb[i * 3 + 1] = paletteRGB[idx * 3 + 1];
        rgb[i * 3 + 2] = paletteRGB[idx * 3 + 2];
    }

    applyScrollSplit(rgb);
    renderSprites(rgb);

    SDL_UpdateTexture(texture, nullptr, rgb.data(), NATIVE_WIDTH * 3);
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, texture, nullptr, nullptr);
    if (debugOverlay) debugOverlay();
    SDL_RenderPresent(renderer);
}

int November::getFPS() {
    return static_cast<int>(std::lround(fps));
}

bool November::shouldQuit() const {
    return shouldExit;
}

void November::requestQuit() {
    shouldExit = true;
}

void November::windowToLogical(float windowX, float windowY, float& logicalX, float& logicalY) const {
    logicalX = windowX;
    logicalY = windowY;
    SDL_RenderCoordinatesFromWindow(renderer, windowX, windowY, &logicalX, &logicalY);
}