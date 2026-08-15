//
// Created by rwarr on 8/7/2026.
//

#include "afterburnerII.h"
#include <cmath>
#include <algorithm>
#include <string>

/*
The Afterburner II audio chip is an AY8930-like chip.
It has 3 square/saw/noise generators, with 4-bit duty cycle and volume.
PSG2 can be combined with PSG3 using a real bitwise operation (AND, NAND, OR, NOR, XOR, XNOR).
PSG3 has the capability to use 8 words (32 4-bit steps) as a wavetable.
The chip runs at 4MHz.

Audio IO:
$9000: PSG Master and PSG1 & PSG2 Volume (0x0V12, Master V, PSG1 vol 1, PSG2 vol 2)
$9001: PSG3 Volume & control (0x0VWO, PSG3 vol V, Wave enable W, unused O)
$9002: PSG waveform select & PSG2 bitwise mode
       (0xM123: M = PSG2 bitwise mode [0 off, 1 AND, 2 NAND, 3 OR, 4 NOR, 5 XOR, 6 XNOR],
       1/2/3 = 2-bit waveform per channel: square, saw, or noise.
       Ignored for PSG3 if wavetable is enabled.)
$9003: PSG duty & saw flips (0x0123 4-bit duty cycle per channel, or flip the saw if not 0)
$9004-$9006: PSG channel tone period
$9007: Reserved
$9010-$9017: PSG3 Wavetable RAM
*/

AfterburnerII::AfterburnerII(Memory &mem, NACPU &cpu, int sample_rate)
    : memory(mem), cpu(cpu), sampleRate(sample_rate) {
}

AfterburnerII::~AfterburnerII() {
    if (stream) {
        SDL_DestroyAudioStream(stream);
        stream = nullptr;
    }
}

int AfterburnerII::init() {
    // Without this, if the real backend (WASAPI on Windows) fails to load
    // for any reason, SDL can silently fall back to its "dummy" audio
    // driver: every call still succeeds, nothing ever plays, and nothing
    // is ever logged. Restricting the candidate list here means that if
    // wasapi/directsound genuinely can't be opened, SDL_InitSubSystem
    // below actually fails and tells us why, instead of quietly going
    // silent.
    SDL_SetHint("SDL_AUDIO_DRIVER", "wasapi,directsound");

    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            SDL_Log("AfterburnerII: SDL_InitSubSystem(AUDIO) failed: %s", SDL_GetError());
            return -1;
        }
    }

    const char* driver = SDL_GetCurrentAudioDriver();
    SDL_Log("AfterburnerII: audio driver = %s", driver ? driver : "(none)");
    if (driver && std::string(driver) == "dummy") {
        // Shouldn't happen given the hint above, but if it does, treat it
        // as a real failure rather than pretending sound is working.
        SDL_Log("AfterburnerII: fell back to the dummy driver - no playback device available.");
        return -1;
    }

    SDL_AudioSpec spec{};
    spec.freq = sampleRate;
    spec.format = SDL_AUDIO_S16;
    spec.channels = 2;

    // nullptr callback = "push" mode: we top the stream up ourselves from
    // update(), rather than SDL pulling from us on its own audio thread.
    // Keeps all register reads on the main/CPU thread, avoiding a data
    // race with Memory (which has no locking).
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream) {
        SDL_Log("AfterburnerII: SDL_OpenAudioDeviceStream failed: %s", SDL_GetError());
        return -1;
    }

    SDL_AudioDeviceID devId = SDL_GetAudioStreamDevice(stream);
    const char* devName = devId ? SDL_GetAudioDeviceName(devId) : nullptr;
    SDL_Log("AfterburnerII: opened playback device '%s'", devName ? devName : "(unknown)");

    if (!SDL_ResumeAudioStreamDevice(stream)) {
        SDL_Log("AfterburnerII: SDL_ResumeAudioStreamDevice failed: %s", SDL_GetError());
        SDL_DestroyAudioStream(stream);
        stream = nullptr;
        return -1;
    }

    audioReady = true;
    return 0;
}

void AfterburnerII::update() {
    if (!audioReady || !stream) return;

    const int bytesPerFrame = 2 * static_cast<int>(sizeof(int16_t)); // stereo S16
    const int targetBytes = (sampleRate * bytesPerFrame * bufferMs) / 1000;

    int queuedBytes = SDL_GetAudioStreamQueued(stream);
    if (queuedBytes < 0) {
        // Device likely disconnected - stop trying rather than spam errors.
        SDL_Log("AfterburnerII: SDL_GetAudioStreamQueued failed: %s", SDL_GetError());
        audioReady = false;
        return;
    }
    if (queuedBytes >= targetBytes) return;

    int neededBytes = targetBytes - queuedBytes;
    int neededSamples = neededBytes / bytesPerFrame;
    if (neededSamples <= 0) return;

    std::vector<int16_t> pcm = generateSamples(neededSamples);
    if (!SDL_PutAudioStreamData(stream, pcm.data(), static_cast<int>(pcm.size() * sizeof(int16_t)))) {
        SDL_Log("AfterburnerII: SDL_PutAudioStreamData failed: %s", SDL_GetError());
    }
}

uint16_t AfterburnerII::readIo(uint16_t addr) {
    return memory.read16("volatile", addr);
}

int16_t AfterburnerII::getWavetableSample(int step_idx) {
    // Extracts a 4-bit nibble (0-15) from the 8-word wavetable at 0x9010-0x9017.
    step_idx = ((step_idx % 32) + 32) % 32;
    uint16_t wordAddr = AUDIO_WAVETABLE + static_cast<uint16_t>(step_idx / 4);
    uint16_t word = readIo(wordAddr);
    int shift = (3 - (step_idx % 4)) * 4;
    return static_cast<int16_t>((word >> shift) & 0x0F);
}

std::vector<int16_t> AfterburnerII::generateSamples(int num_samples) {
    if (num_samples <= 0) return {};

    std::vector<float> left(static_cast<size_t>(num_samples), 0.0f);
    std::vector<float> right(static_cast<size_t>(num_samples), 0.0f);

    // 1. Read control registers
    uint16_t v012 = readIo(AUDIO_PSG_VOL012);
    int masterVol = (v012 >> 8) & 0x0F;
    int psgVol[3];
    psgVol[0] = (v012 >> 4) & 0x0F;
    psgVol[1] = v012 & 0x0F;

    uint16_t v3ctrl = readIo(AUDIO_PSG3_CTRL);
    psgVol[2] = (v3ctrl >> 8) & 0x0F;
    bool waveEnable = ((v3ctrl >> 4) & 0x0F) != 0;

    uint16_t waveSel = readIo(AUDIO_PSG_WAVE);
    int waves[3] = {
        (waveSel >> 8) & 0x03,
        (waveSel >> 4) & 0x03,
        waveSel & 0x03
    };
    // PSG2 bitwise-combine-with-PSG3 mode: 0 off, 1 AND, 2 NAND, 3 OR, 4 NOR, 5 XOR, 6 XNOR
    int psg2Bitwise = (waveSel >> 12) & 0x0F;
    if (psg2Bitwise > 6) psg2Bitwise = 6;

    uint16_t dutyReg = readIo(AUDIO_PSG_DUTY);
    int duties[3] = {
        (dutyReg >> 8) & 0x0F,
        (dutyReg >> 4) & 0x0F,
        dutyReg & 0x0F
    };

    int periods[3] = {
        readIo(AUDIO_PSG_TONE0),
        readIo(AUDIO_PSG_TONE1),
        readIo(AUDIO_PSG_TONE2)
    };

    float mGain = masterVol / 15.0f;

    // 2. Synthesize PSG channels. Generate raw bipolar signals first so
    // PSG2 can be combined with PSG3 before volume/mix.
    std::vector<std::vector<float>> raw(3);
    bool active[3] = {false, false, false};

    for (int ch = 0; ch < 3; ++ch) {
        raw[ch].assign(static_cast<size_t>(num_samples), 0.0f);

        int period = periods[ch];
        if (period == 0) continue;
        active[ch] = true;

        double freq = static_cast<double>(CHIP_CLOCK_HZ) / (64.0 * period);
        double phaseStep = freq / sampleRate;
        int wtype = waves[ch];
        float duty = duties[ch] > 0 ? duties[ch] / 15.0f : 0.5f;

        double phase = psg_phase[static_cast<size_t>(ch)];

        if (ch == 2 && waveEnable) {
            // PSG3 wavetable lookup (4-bit steps -> bipolar). Sample i uses
            // the phase *before* advancing, matching the reference chip's
            // vectorized "initial + i*step" positional lookup.
            for (int i = 0; i < num_samples; ++i) {
                int idx = static_cast<int>(phase * 32.0) % 32;
                int16_t nibble = getWavetableSample(idx);
                raw[ch][static_cast<size_t>(i)] = (nibble / 15.0f) * 2.0f - 1.0f;
                phase = std::fmod(phase + phaseStep, 1.0);
            }
        } else if (wtype == 2) {
            // Noise: 1-bit output, re-rolled once per phase wrap (clocked by
            // the channel's tone period like the other waveforms), biased by
            // duty just like the square wave's pulse width.
            float bit = noise_bit[static_cast<size_t>(ch)];
            for (int i = 0; i < num_samples; ++i) {
                double prevPhase = phase;
                phase = std::fmod(phase + phaseStep, 1.0);
                if (phase < prevPhase || i == 0) {
                    bit = (uni(rng) < duty) ? 1.0f : -1.0f;
                }
                raw[ch][static_cast<size_t>(i)] = bit;
            }
            noise_bit[static_cast<size_t>(ch)] = bit;
        } else {
            for (int i = 0; i < num_samples; ++i) {
                if (wtype == 0) { // square
                    raw[ch][static_cast<size_t>(i)] = (phase < duty) ? 1.0f : -1.0f;
                } else { // sawtooth
                    float saw = static_cast<float>(phase) * 2.0f - 1.0f;
                    if (duties[ch] != 0) saw = -saw; // saw flip
                    raw[ch][static_cast<size_t>(i)] = saw;
                }
                phase = std::fmod(phase + phaseStep, 1.0);
            }
        }

        psg_phase[static_cast<size_t>(ch)] = static_cast<float>(phase);
    }

    // Apply PSG2 bitwise-combine with PSG3 when enabled. Quantize both
    // channels' current amplitude to the chip's native 4-bit resolution
    // and combine them with a real bitwise op, sample by sample.
    if (psg2Bitwise != 0 && active[1] && active[2]) {
        for (int i = 0; i < num_samples; ++i) {
            int v2q = std::clamp(static_cast<int>((raw[1][static_cast<size_t>(i)] * 0.5f + 0.5f) * 15.0f + 0.5f), 0, 15);
            int v3q = std::clamp(static_cast<int>((raw[2][static_cast<size_t>(i)] * 0.5f + 0.5f) * 15.0f + 0.5f), 0, 15);
            int combined;
            switch (psg2Bitwise) {
                case 1: combined = v2q & v3q; break;                    // AND
                case 2: combined = (~(v2q & v3q)) & 0x0F; break;        // NAND
                case 3: combined = v2q | v3q; break;                    // OR
                case 4: combined = (~(v2q | v3q)) & 0x0F; break;        // NOR
                case 5: combined = v2q ^ v3q; break;                    // XOR
                default: combined = (~(v2q ^ v3q)) & 0x0F; break;       // XNOR
            }
            raw[1][static_cast<size_t>(i)] = (combined / 15.0f) * 2.0f - 1.0f;
        }
    }

    // 3. Apply volume and mix (0.25 gain per channel to match native headroom)
    for (int ch = 0; ch < 3; ++ch) {
        if (!active[ch]) continue;
        float vol = (psgVol[ch] / 15.0f) * mGain;
        for (int i = 0; i < num_samples; ++i) {
            float s = raw[ch][static_cast<size_t>(i)] * vol;
            left[static_cast<size_t>(i)]  += s * 0.25f;
            right[static_cast<size_t>(i)] += s * 0.25f;
        }
    }

    // 4. Convert float to interleaved int16 PCM
    std::vector<int16_t> out(static_cast<size_t>(num_samples) * 2);
    for (int i = 0; i < num_samples; ++i) {
        float l = std::clamp(left[static_cast<size_t>(i)], -1.0f, 1.0f) * 32767.0f;
        float r = std::clamp(right[static_cast<size_t>(i)], -1.0f, 1.0f) * 32767.0f;
        out[static_cast<size_t>(i) * 2 + 0] = static_cast<int16_t>(l);
        out[static_cast<size_t>(i) * 2 + 1] = static_cast<int16_t>(r);
    }
    return out;
}