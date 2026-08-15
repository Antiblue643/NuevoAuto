//
// Created by rwarr on 8/8/2026.
//

#include "debugger.h"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <cstdio>
#include <cstdlib>
#include <algorithm>

Debugger::Debugger(Memory& mem, NACPU& cpuRef, November& nov)
    : memory(mem), cpu(cpuRef), display(nov)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    // Nav + take exclusive ownership of the OS cursor. Without
    // NoMouseCursorChange, imgui_impl_sdl3's NewFrame always calls
    // SDL_ShowCursor() (default Arrow), undoing any HideCursor we do
    // and leaving the cursor permanently visible even with the overlay closed.
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard
                                | ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::StyleColorsDark();

    ImGui_ImplSDL3_InitForSDLRenderer(nov.getWindow(), nov.getRenderer());
    ImGui_ImplSDLRenderer3_Init(nov.getRenderer());

    std::snprintf(writeAddrBuf, sizeof(writeAddrBuf), "%s", "");
    std::snprintf(writeValBuf, sizeof(writeValBuf), "%s", "");

    // Install the overlay hook: November calls this every update(), right
    // before it presents the frame.
    nov.setDebugOverlay([this] { render(); });
}

Debugger::~Debugger() {
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

void Debugger::processEvent(const SDL_Event& event) {
    ImGui_ImplSDL3_ProcessEvent(&event);
}   

bool Debugger::wantsKeyboard() const {
    return visible && ImGui::GetIO().WantCaptureKeyboard;
}

bool Debugger::wantsMouse() const {
    return visible && ImGui::GetIO().WantCaptureMouse;
}

void Debugger::toggle() {
    visible = !visible;
    // Apply immediately so the cursor state matches before the next
    // November::update() / render() cycle (avoids a one-frame flash).
    if (visible) {
        SDL_ShowCursor();
    } else {
        SDL_HideCursor();
    }
}

// ---------------------------------------------------------------------------
// Helpers (ports of debugger.py's _write_memory / _decode_instruction / _regs_summary)
// ---------------------------------------------------------------------------
void Debugger::writeMemory() {
    statusMsg.clear();
    char* addrEnd = nullptr;
    char* valEnd = nullptr;
    long addr = std::strtol(writeAddrBuf, &addrEnd, 16);
    long val = std::strtol(writeValBuf, &valEnd, 16);

    if (addrEnd == writeAddrBuf || valEnd == writeValBuf) {
        statusMsg = "Invalid input or bounds.";
        return;
    }

    memory.write16(region, static_cast<size_t>(addr), static_cast<uint16_t>(val));
    recentWrites[{region, static_cast<int>(addr)}] = ImGui::GetTime() + WRITE_TTL;

    char buf[64];
    std::snprintf(buf, sizeof(buf), "Success: %02X -> %06X",
                  static_cast<unsigned>(val) & 0xFFFFu, static_cast<unsigned>(addr) & 0xFFFFFFu);
    statusMsg = buf;
}

std::string Debugger::decodeInstruction(int pc) const {
    char buf[160];
    uint16_t opcodeWord = memory.read16("nprog", static_cast<size_t>(pc));
    if (opcodeWord >= OPCODES.size()) {
        std::snprintf(buf, sizeof(buf), "PC=0x%04X  ??? (0x%02X)", pc, opcodeWord);
        return buf;
    }

    std::string name(OPCODES[opcodeWord]);
    auto it = OPERAND_COUNTS.find(name);
    int count = (it != OPERAND_COUNTS.end()) ? it->second : 0;

    std::string opStr;
    for (int i = 0; i < count; ++i) {
        uint16_t o = memory.read16("nprog", static_cast<size_t>(pc + 1 + i));
        char tok[8];
        std::snprintf(tok, sizeof(tok), "%02X", o);
        if (i) opStr += " ";
        opStr += tok;
    }

    const char* status = cpu.isRunning() ? "" : "  [HALTED]";
    std::snprintf(buf, sizeof(buf), "PC=0x%04X  %s %s%s", pc, name.c_str(), opStr.c_str(), status);
    return buf;
}

std::string Debugger::regsSummary() const {
    const auto& regs = cpu.getRegisters();
    std::string out;
    char tok[16];
    for (int i = 0; i < 16; ++i) {
        std::snprintf(tok, sizeof(tok), "R%X=%02X  ", i, static_cast<uint16_t>(regs[i]));
        out += tok;
    }
    out += "  flags:";
    out += cpu.getCarry() ? "C" : "-";
    return out;
}

// ---------------------------------------------------------------------------
// Frame render - always called once per November::update(), regardless of
// visible, to keep ImGui's NewFrame/Render pairing consistent.
// ---------------------------------------------------------------------------
void Debugger::render() {
    // Temporarily disable SDL logical presentation so ImGui renders in full
    // window-pixel space. With LETTERBOX active the overlay is forced into the
    // 320x240 logical viewport (appearing massive / clipped) and mouse coords
    // are offset by the letterbox margins, especially after resize.
    SDL_Renderer* ren = display.getRenderer();
    int savedW = 0, savedH = 0;
    SDL_RendererLogicalPresentation savedMode = SDL_LOGICAL_PRESENTATION_DISABLED;
    SDL_GetRenderLogicalPresentation(ren, &savedW, &savedH, &savedMode);
    SDL_SetRenderLogicalPresentation(ren, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);

    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    if (visible) {
        const double now = ImGui::GetTime();
        for (auto it = recentWrites.begin(); it != recentWrites.end(); ) {
            if (it->second <= now) it = recentWrites.erase(it);
            else ++it;
        }

        ImGui::SetNextWindowSize(ImVec2(760, 560), ImGuiCond_FirstUseEver);
        ImGui::Begin("NACPU Live Memory Viewer", &visible);

        // --- Region + view offset -------------------------------------
        if (ImGui::BeginCombo("Region", region.c_str())) {
            for (const auto& kv : memory.mem_regions) {
                bool selected = (kv.first == region);
                if (ImGui::Selectable(kv.first.c_str(), selected)) region = kv.first;
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("<<")) {
            offset = std::max(0, offset - 256);
            std::snprintf(offsetBuf, sizeof(offsetBuf), "%04X", offset);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        if (ImGui::InputText("Offset", offsetBuf, sizeof(offsetBuf),
                              ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue)) {
            offset = static_cast<int>(std::strtol(offsetBuf, nullptr, 16));
        }
        if (offset < 0) { offset = 0; std::snprintf(offsetBuf, sizeof(offsetBuf), "0000"); }
        ImGui::SameLine();
        if (ImGui::Button(">>")) {
            offset += 256;
            std::snprintf(offsetBuf, sizeof(offsetBuf), "%04X", offset);
        }

        // --- Write control -----------------------------------------------
        ImGui::Separator();
        ImGui::SetNextItemWidth(80);
        ImGui::InputText("Write Addr", writeAddrBuf, sizeof(writeAddrBuf), ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::InputText("Val", writeValBuf, sizeof(writeValBuf), ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::SameLine();
        if (ImGui::Button("Write (Hex)")) writeMemory();
        if (!statusMsg.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.0f, 0.67f, 1.0f, 1.0f), "%s", statusMsg.c_str());
        }

        // --- Instruction / registers --------------------------------------
        ImGui::Separator();
        int pc = cpu.getPC() & 0xFFFF;
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s", decodeInstruction(pc).c_str());
        ImGui::TextColored(ImVec4(0.61f, 0.86f, 0.996f, 1.0f), "%s", regsSummary().c_str());
        ImGui::Separator();

        // --- Hex dump ------------------------------------------------------
        auto regionIt = memory.mem_regions.find(region);
        if (regionIt == memory.mem_regions.end()) {
            ImGui::TextUnformatted("Invalid memory region.");
        } else {
            std::vector<uint16_t>* vec = regionIt->second;
            int size = static_cast<int>(vec->size());
            offset = std::max(0, std::min(offset, std::max(0, size - 256)));
            int length = std::min(256, size - offset);

            std::vector<uint16_t> data(static_cast<size_t>(length));
            for (int i = 0; i < length; ++i) data[static_cast<size_t>(i)] = (*vec)[static_cast<size_t>(offset + i)];

            // Detect changes vs the last-drawn same window (mirrors debugger.py).
            if (prevRegion == region && prevOffset == offset && prevData.size() == data.size()) {
                for (int i = 0; i < length; ++i) {
                    if (prevData[static_cast<size_t>(i)] != data[static_cast<size_t>(i)]) {
                        recentWrites[{region, offset + i}] = now + WRITE_TTL;
                    }
                }
            }
            prevRegion = region;
            prevOffset = offset;
            prevData = data;

            int pcLocal = (region == "nprog" && pc >= offset && pc < offset + length) ? (pc - offset) : -1;

            ImGui::BeginChild("hexdump", ImVec2(0, 0), true);
            ImGui::TextUnformatted("Offset   00   01   02   03   04   05   06   07   08   09   0A   0B   0C   0D   0E   0F   ASCII");
            ImGui::Separator();

            for (int row = 0; row < length; row += 16) {
                int absBase = offset + row;
                ImGui::Text("%06X", absBase);
                std::string ascii;
                for (int col = 0; col < 16; ++col) {
                    int localIdx = row + col;
                    ImGui::SameLine();
                    if (localIdx >= length) {
                        ImGui::TextUnformatted("     ");
                        continue;
                    }
                    int absAddr = offset + localIdx;
                    uint16_t val = data[static_cast<size_t>(localIdx)];
                    ImVec4 color(0.83f, 0.83f, 0.83f, 1.0f);
                    if (localIdx == pcLocal) color = ImVec4(0.85f, 0.65f, 0.05f, 1.0f);
                    else if (recentWrites.count({region, absAddr})) color = ImVec4(0.85f, 0.15f, 0.15f, 1.0f);
                    ImGui::TextColored(color, "%04X", val);
                    ascii += (val >= 32 && val <= 126) ? static_cast<char>(val) : '.';
                }
                ImGui::SameLine();
                ImGui::TextUnformatted(("  |" + ascii + "|").c_str());
            }
            ImGui::EndChild();
        }
        ImGui::End();
    }

    ImGui::Render();
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), ren);

    // Restore the emulator's logical presentation (letterbox 320x240).
    SDL_SetRenderLogicalPresentation(ren, savedW, savedH, savedMode);

    // Own the OS cursor ourselves (ImGuiConfigFlags_NoMouseCursorChange is
    // set so the SDL3 backend won't fight us). Show only while the overlay
    // is open so the user can click ImGui widgets; keep it hidden otherwise.
    if (visible) {
        SDL_ShowCursor();
    } else {
        SDL_HideCursor();
    }

    if (!visible || !ImGui::GetIO().WantTextInput) {
        SDL_StartTextInput(display.getWindow());
    }
}