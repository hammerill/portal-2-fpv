#include "audio.h"

#if defined(_WIN32)
#include <windows.h>
#include <mmsystem.h>

#include <atomic>
#include <cmath>

#include "fpvapi.h"
#include "log.h"

namespace audio {
namespace {

using fpvapi::fpv;

constexpr uint32_t kRate = 44100, kChannels = 2;
constexpr uint32_t kFrames = 512;  // per buffer: ~12 ms
constexpr int kBuffers = 4;        // ~46 ms queued

HANDLE g_thread = nullptr;
std::atomic<bool> g_run{false};
std::atomic<bool> g_static{false};
constexpr float kStaticLevel = 0.06f;  // the motors are around 0.04 RMS

DWORD WINAPI thread_main(void *) {
    HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = kChannels;
    fmt.nSamplesPerSec = kRate;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = fmt.nChannels * fmt.wBitsPerSample / 8;
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
    HWAVEOUT out = nullptr;
    MMRESULT mr = waveOutOpen(&out, WAVE_MAPPER, &fmt, DWORD_PTR(ev), 0, CALLBACK_EVENT);
    if (mr != MMSYSERR_NOERROR) {
        fpvlog::write("audio: waveOutOpen failed (%u)", unsigned(mr));
        CloseHandle(ev);
        return 0;
    }
    static short pcm[kBuffers][kFrames * kChannels];
    static float mix[kFrames * kChannels];
    WAVEHDR hdr[kBuffers]{};
    for (int i = 0; i < kBuffers; ++i) {
        hdr[i].lpData = reinterpret_cast<LPSTR>(pcm[i]);
        hdr[i].dwBufferLength = sizeof pcm[i];
        waveOutPrepareHeader(out, &hdr[i], sizeof hdr[i]);
        hdr[i].dwFlags |= WHDR_DONE;  // free to fill
    }
    while (g_run) {
        for (int i = 0; i < kBuffers; ++i) {
            if (!(hdr[i].dwFlags & WHDR_DONE)) continue;
            fpv.fpv_audio_read(mix, kFrames);
            bool hiss = g_static;
            static uint32_t seed = 0x9e3779b9u;
            for (uint32_t s = 0; s < kFrames * kChannels; ++s) {
                float v = std::isfinite(mix[s]) ? mix[s] : 0.0f;
                if (hiss) {  // white noise, xorshift32
                    seed ^= seed << 13, seed ^= seed >> 17, seed ^= seed << 5;
                    v += (float(seed >> 8) / 8388608.0f - 1.0f) * kStaticLevel;
                }
                v = v < -1 ? -1 : v > 1 ? 1 : v;
                pcm[i][s] = short(v * 32767.0f);
            }
            hdr[i].dwFlags &= ~WHDR_DONE;
            waveOutWrite(out, &hdr[i], sizeof hdr[i]);
        }
        WaitForSingleObject(ev, 50);
    }
    waveOutReset(out);
    for (int i = 0; i < kBuffers; ++i) waveOutUnprepareHeader(out, &hdr[i], sizeof hdr[i]);
    waveOutClose(out);
    CloseHandle(ev);
    return 0;
}

}  // namespace

void start() {
    if (g_thread || !fpvapi::loaded()) return;
    fpv.fpv_audio_configure(kRate, kChannels);
    g_run = true;
    g_thread = CreateThread(nullptr, 0, &thread_main, nullptr, 0, nullptr);
    fpvlog::write("audio: %s", g_thread ? "started" : "thread creation failed");
}

void set_static(bool on) { g_static = on; }

void stop() {
    g_static = false;
    if (!g_thread) return;
    g_run = false;
    WaitForSingleObject(g_thread, 2000);
    CloseHandle(g_thread);
    g_thread = nullptr;
    fpvlog::write("audio: stopped");
}

}  // namespace audio

#else  // macOS: an AudioQueue pulls libfpv's mix on its own thread.

#include <AudioToolbox/AudioToolbox.h>

#include <algorithm>
#include <atomic>
#include <cmath>

#include "fpvapi.h"
#include "log.h"

namespace audio {
namespace {

using fpvapi::fpv;

constexpr uint32_t kRate = 44100, kChannels = 2;
constexpr uint32_t kFrames = 512;  // per buffer: ~12 ms
constexpr int kBuffers = 4;        // ~46 ms queued
constexpr float kStaticLevel = 0.06f;  // the motors are around 0.04 RMS

AudioQueueRef g_queue = nullptr;
std::atomic<bool> g_static{false};
std::atomic<bool> g_run{false};

void fill(void *, AudioQueueRef queue, AudioQueueBufferRef buffer) {
    static float mix[kFrames * kChannels];
    auto *pcm = static_cast<int16_t *>(buffer->mAudioData);
    if (g_run) fpv.fpv_audio_read(mix, kFrames);
    else std::fill(mix, mix + kFrames * kChannels, 0.0f);
    bool hiss = g_static;
    static uint32_t seed = 0x9e3779b9u;
    for (uint32_t s = 0; s < kFrames * kChannels; ++s) {
        float v = std::isfinite(mix[s]) ? mix[s] : 0.0f;
        if (hiss) {  // white noise, xorshift32
            seed ^= seed << 13, seed ^= seed >> 17, seed ^= seed << 5;
            v += (float(seed >> 8) / 8388608.0f - 1.0f) * kStaticLevel;
        }
        v = v < -1 ? -1 : v > 1 ? 1 : v;
        pcm[s] = int16_t(v * 32767.0f);
    }
    buffer->mAudioDataByteSize = kFrames * kChannels * sizeof(int16_t);
    if (g_run) AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
}

}  // namespace

void start() {
    if (g_queue || !fpvapi::loaded()) return;
    fpv.fpv_audio_configure(kRate, kChannels);
    AudioStreamBasicDescription fmt{};
    fmt.mSampleRate = kRate;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    fmt.mChannelsPerFrame = kChannels;
    fmt.mBitsPerChannel = 16;
    fmt.mBytesPerFrame = kChannels * 2;
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerPacket = fmt.mBytesPerFrame;
    // its own thread for the callbacks (nullptr run loop): the game's main thread is busy
    OSStatus st = AudioQueueNewOutput(&fmt, &fill, nullptr, nullptr, nullptr, 0, &g_queue);
    if (st != noErr) {
        fpvlog::write("audio: AudioQueueNewOutput failed (%d)", int(st));
        g_queue = nullptr;
        return;
    }
    g_run = true;
    for (int i = 0; i < kBuffers; ++i) {
        AudioQueueBufferRef b = nullptr;
        if (AudioQueueAllocateBuffer(g_queue, kFrames * kChannels * sizeof(int16_t), &b) == noErr) fill(nullptr, g_queue, b);
    }
    st = AudioQueueStart(g_queue, nullptr);
    fpvlog::write("audio: %s", st == noErr ? "started" : "AudioQueueStart failed");
}

void set_static(bool on) { g_static = on; }

void stop() {
    g_static = false;
    if (!g_queue) return;
    g_run = false;
    AudioQueueStop(g_queue, true);
    AudioQueueDispose(g_queue, true);
    g_queue = nullptr;
    fpvlog::write("audio: stopped");
}

}  // namespace audio

#endif
