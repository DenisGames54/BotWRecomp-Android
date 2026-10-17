#include "output.h"

#include <SDL3/SDL.h>
#include <math.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

#include "../log.h"
#include "../os.h"

extern bool g_mute;

namespace audio {
namespace {
std::once_flag g_once;
SDL_AudioStream* g_stream = nullptr;
std::vector<int16_t> g_stereo;
uint64_t g_frames_total = 0;
double g_sum_sq = 0;
uint64_t g_level_frames = 0;
}  // namespace

void init() {
    std::call_once(g_once, [] {
        if (g_headless || g_mute) {
            LOG("audio: muted");
            return;
        }
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            LOG("audio: SDL audio init failed: %s", SDL_GetError());
            return;
        }
        SDL_AudioSpec spec{SDL_AUDIO_S16, 2, 48000};
        g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!g_stream) {
            LOG("audio: no output device: %s", SDL_GetError());
            return;
        }
        SDL_ResumeAudioStreamDevice(g_stream);
        LOG("audio: output 48 kHz stereo");
    });
}

std::atomic<float> g_volume{1.0f};
void set_volume(float v) { g_volume = std::clamp(v, 0.0f, 1.0f); }

void push_tv(const int16_t* in, int frames, int channels) {
    const float vol = g_volume;
    g_stereo.resize((size_t)frames * 2);
    for (int i = 0; i < frames; i++) {
        int l, r;
        if (channels >= 6) {
            // L R C LFE Ls Rs -> stereo
            const int16_t* s = in + i * channels;
            l = (int)(s[0] + 0.707f * s[2] + 0.707f * s[4]);
            r = (int)(s[1] + 0.707f * s[2] + 0.707f * s[5]);
        } else if (channels == 2) {
            l = in[i * 2];
            r = in[i * 2 + 1];
        } else {
            l = r = in[i * channels];
        }
        g_stereo[i * 2] = (int16_t)std::clamp((int)(l * vol), -32768, 32767);
        g_stereo[i * 2 + 1] = (int16_t)std::clamp((int)(r * vol), -32768, 32767);
        g_sum_sq += (double)l * l;
    }
    g_frames_total += frames;
    g_level_frames += frames;
    if (g_level_frames >= 48000 * 10) {
        double rms = sqrt(g_sum_sq / g_level_frames) / 32768.0;
        LOG("audio: %.1f dBFS over the last 10 s", rms > 0 ? 20 * log10(rms) : -120.0);
        g_level_frames = 0;
        g_sum_sq = 0;
    }
    if (g_stream) SDL_PutAudioStreamData(g_stream, g_stereo.data(), frames * 4);
}

int queued_frames() { return g_stream ? SDL_GetAudioStreamQueued(g_stream) / 4 : 0; }
}  // namespace audio
