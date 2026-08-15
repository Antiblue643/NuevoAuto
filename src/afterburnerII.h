//
// Created by rwarr on 8/7/2026.
//

#ifndef NUEVOAUTO_AFTERBURNERII_H
#define NUEVOAUTO_AFTERBURNERII_H

#define AUDIO_IO_BASE      0x9000
#define AUDIO_PSG_VOL012   0x9000  // 0x0V12: Master V, PSG1 vol 1, PSG2 vol 2
#define AUDIO_PSG3_CTRL    0x9001  // 0x0VWO: PSG3 vol V, Wave enable W
#define AUDIO_PSG_WAVE     0x9002  // 0xM123: M=PSG2 bitwise mode, 1/2/3 = 2-bit waveform per channel
#define AUDIO_PSG_DUTY     0x9003  // 0x0123: 4-bit duty cycle / saw flip per channel
#define AUDIO_PSG_TONE0    0x9004  // PSG1 period
#define AUDIO_PSG_TONE1    0x9005  // PSG2 period
#define AUDIO_PSG_TONE2    0x9006  // PSG3 period
#define AUDIO_WAVETABLE    0x9010  // 0x9010-0x9017: 8 words = 32 x 4-bit steps

#define CHIP_CLOCK_HZ 4000000      // 4 MHz

#include "NACPU.h"
#include "memory.h"
#include <SDL3/SDL.h>
#include <random>
#include <vector>
#include <cstdint>

class AfterburnerII {
public:
    AfterburnerII(Memory &mem, NACPU &cpu, int sample_rate);
    ~AfterburnerII();

    // Opens the default playback device and starts an SDL3 audio stream
    // at the sample rate given to the constructor (stereo, S16). Returns
    // 0 on success, -1 on failure (audio is non-fatal - the machine can
    // run without sound if this fails, so callers should just log it).
    int init();

    // Call once per frame (e.g. right after VMV FLP / vblank) to top the
    // device's internal queue back up to ~bufferMs of audio, generating
    // only as many samples as are needed to refill it. Cheap no-op if
    // init() wasn't called or already failed.
    void update();

    // Per-channel phase accumulators (0-1) and latched noise bit (-1/1),
    // persisted across generateSamples() calls so waveforms stay
    // continuous from one audio buffer to the next.
    std::vector<float> psg_phase = {0.0, 0.0, 0.0};
    std::vector<float> noise_bit = {1.0, 1.0, 1.0};

    int16_t getWavetableSample(int step_idx);

    // Returns interleaved stereo PCM16 [L0,R0,L1,R1,...], size num_samples*2.
    std::vector<int16_t> generateSamples(int num_samples);

private:
    Memory& memory;
    NACPU& cpu; // unused by the chip itself today, kept for future IRQ/sync hooks
    int sampleRate;
    std::mt19937 rng{std::random_device{}()};
    std::uniform_real_distribution<float> uni{0.0f, 1.0f};

    SDL_AudioStream* stream = nullptr;
    bool audioReady = false;
    int bufferMs = 100; // target queued latency

    uint16_t readIo(uint16_t addr);
};

#endif //NUEVOAUTO_AFTERBURNERII_H
