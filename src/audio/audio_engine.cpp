#include "audio/audio_engine.h"
#include "util/log.h"

#include <SDL3/SDL.h>
#include <cmath>
#include <cstring>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace bro::audio {

AudioEngine::AudioEngine() = default;

AudioEngine::~AudioEngine()
{
    shutdown();
}

bool AudioEngine::init()
{
    if (initialized_) return true;

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        LOG_ERROR("Failed to init SDL audio: %s", SDL_GetError());
        return false;
    }

    SDL_AudioSpec spec;
    spec.format = SDL_AUDIO_F32;
    spec.channels = 1;
    spec.freq = sampleRate_;

    stream_ = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
        &spec, audioCallback, this);

    if (!stream_) {
        LOG_ERROR("Failed to open audio device: %s", SDL_GetError());
        return false;
    }

    SDL_ResumeAudioStreamDevice(stream_);
    initialized_ = true;
    LOG_INFO("Audio engine initialized: %d Hz mono", sampleRate_);
    return true;
}

void AudioEngine::shutdown()
{
    if (stream_) {
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
    }
    initialized_ = false;
}

double AudioEngine::currentTime() const
{
    return static_cast<double>(samplesGenerated_.load(std::memory_order_relaxed))
           / static_cast<double>(sampleRate_);
}

int AudioEngine::createVoice()
{
    std::lock_guard<std::mutex> lock(mutex_);
    int id = nextVoiceId_++;
    Voice v;
    v.id = id;
    voices_.push_back(v);
    return id;
}

void AudioEngine::removeVoice(int id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    voices_.erase(
        std::remove_if(voices_.begin(), voices_.end(),
                       [id](const Voice& v) { return v.id == id; }),
        voices_.end());
}

Voice* AudioEngine::findVoice(int id)
{
    for (auto& v : voices_) {
        if (v.id == id) return &v;
    }
    return nullptr;
}

void AudioEngine::setWaveform(int id, Waveform wf)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto* v = findVoice(id)) v->waveform = wf;
}

void AudioEngine::setFrequency(int id, float freq)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto* v = findVoice(id)) v->frequency = freq;
}

void AudioEngine::setGain(int id, float gain)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto* v = findVoice(id)) v->gain = gain;
}

void AudioEngine::startVoice(int id, double when)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto* v = findVoice(id)) {
        v->startTime = when;
        v->started = true;
        v->active = true;
        v->phase = 0.0f;
    }
}

void AudioEngine::stopVoice(int id, double when)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto* v = findVoice(id)) {
        v->stopTime = when;
    }
}

void AudioEngine::audioCallback(void* userdata, SDL_AudioStream* stream,
                                int additional_amount, int /*total_amount*/)
{
    auto* engine = static_cast<AudioEngine*>(userdata);
    int numSamples = additional_amount / static_cast<int>(sizeof(float));
    if (numSamples <= 0) return;

    // Use a stack buffer for small requests, heap for large
    float stackBuf[4096];
    float* buffer = (numSamples <= 4096) ? stackBuf : new float[numSamples];

    std::memset(buffer, 0, numSamples * sizeof(float));
    engine->generateSamples(buffer, numSamples);

    SDL_PutAudioStreamData(stream, buffer, numSamples * sizeof(float));

    if (buffer != stackBuf) delete[] buffer;
}

void AudioEngine::generateSamples(float* buffer, int numSamples)
{
    std::lock_guard<std::mutex> lock(mutex_);

    double baseTime = static_cast<double>(samplesGenerated_.load(std::memory_order_relaxed))
                      / static_cast<double>(sampleRate_);
    double sampleDt = 1.0 / static_cast<double>(sampleRate_);

    for (auto& voice : voices_) {
        if (!voice.active || !voice.started) continue;

        float freq = voice.frequency;
        float gain = voice.gain;
        float phaseInc = freq / static_cast<float>(sampleRate_);

        for (int i = 0; i < numSamples; i++) {
            double t = baseTime + i * sampleDt;

            // Check start/stop times
            if (t < voice.startTime) continue;
            if (voice.stopTime >= 0 && t >= voice.stopTime) {
                voice.active = false;
                break;
            }

            float sample = 0.0f;
            switch (voice.waveform) {
                case Waveform::Sine:
                    sample = std::sin(voice.phase * 2.0f * static_cast<float>(M_PI));
                    break;
                case Waveform::Square:
                    sample = (voice.phase < 0.5f) ? 1.0f : -1.0f;
                    break;
                case Waveform::Sawtooth:
                    sample = 2.0f * voice.phase - 1.0f;
                    break;
                case Waveform::Triangle:
                    sample = (voice.phase < 0.5f)
                        ? (4.0f * voice.phase - 1.0f)
                        : (3.0f - 4.0f * voice.phase);
                    break;
            }

            buffer[i] += sample * gain;
            voice.phase += phaseInc;
            if (voice.phase >= 1.0f) voice.phase -= 1.0f;
        }
    }

    // Clamp output
    for (int i = 0; i < numSamples; i++) {
        buffer[i] = std::clamp(buffer[i], -1.0f, 1.0f);
    }

    // Clean up finished voices
    voices_.erase(
        std::remove_if(voices_.begin(), voices_.end(),
                       [](const Voice& v) { return v.started && !v.active; }),
        voices_.end());

    samplesGenerated_.fetch_add(static_cast<uint64_t>(numSamples), std::memory_order_relaxed);
}

} // namespace bro::audio
