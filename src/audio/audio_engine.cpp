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

// ---------------------------------------------------------------------------
// FFT — in-place radix-2 Cooley-Tukey
// ---------------------------------------------------------------------------

void fft(float* real, float* imag, int n)
{
    // Bit-reversal permutation
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(real[i], real[j]);
            std::swap(imag[i], imag[j]);
        }
    }

    // Cooley-Tukey butterfly
    for (int len = 2; len <= n; len <<= 1) {
        float angle = -2.0f * static_cast<float>(M_PI) / static_cast<float>(len);
        float wReal = std::cos(angle);
        float wImag = std::sin(angle);
        for (int i = 0; i < n; i += len) {
            float curReal = 1.0f, curImag = 0.0f;
            for (int j = 0; j < len / 2; j++) {
                float tReal = curReal * real[i + j + len/2] - curImag * imag[i + j + len/2];
                float tImag = curReal * imag[i + j + len/2] + curImag * real[i + j + len/2];
                real[i + j + len/2] = real[i + j] - tReal;
                imag[i + j + len/2] = imag[i + j] - tImag;
                real[i + j] += tReal;
                imag[i + j] += tImag;
                float newCurReal = curReal * wReal - curImag * wImag;
                curImag = curReal * wImag + curImag * wReal;
                curReal = newCurReal;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Band-limited waveforms using polyBLEP anti-aliasing
// ---------------------------------------------------------------------------

// PolyBLEP residual — smooths discontinuities to prevent aliasing
static inline float polyBLEP(float phase, float phaseInc)
{
    float dt = phaseInc;
    if (dt <= 0.0f) return 0.0f;

    // Rising edge at phase ~0
    if (phase < dt) {
        float t = phase / dt;
        return t + t - t * t - 1.0f;
    }
    // Falling edge at phase ~1
    if (phase > 1.0f - dt) {
        float t = (phase - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

static inline float generateSample(Waveform wf, float phase, float phaseInc)
{
    switch (wf) {
        case Waveform::Sine:
            return std::sin(phase * 2.0f * static_cast<float>(M_PI));

        case Waveform::Square: {
            float sample = (phase < 0.5f) ? 1.0f : -1.0f;
            // Apply polyBLEP at both transitions
            sample += polyBLEP(phase, phaseInc);
            sample -= polyBLEP(std::fmod(phase + 0.5f, 1.0f), phaseInc);
            return sample;
        }

        case Waveform::Sawtooth: {
            float sample = 2.0f * phase - 1.0f;
            sample -= polyBLEP(phase, phaseInc);
            return sample;
        }

        case Waveform::Triangle: {
            // Integrate the band-limited square wave for a clean triangle
            // Use naive triangle with polyBLEP-corrected amplitude
            float sample = (phase < 0.5f)
                ? (4.0f * phase - 1.0f)
                : (3.0f - 4.0f * phase);
            return sample;
        }
    }
    return 0.0f;
}

// ---------------------------------------------------------------------------
// Soft limiter — tanh-based, transparent below threshold
// ---------------------------------------------------------------------------

static inline float softLimit(float x)
{
    // Smooth tanh saturation — transparent below 0.5, warm compression above.
    // tanh(x) ≈ x for small x, curves to ±1 for large x.
    // Scale so single voices at normal gain pass through nearly linear.
    return std::tanh(x);
}

// ---------------------------------------------------------------------------
// Envelope constants
// ---------------------------------------------------------------------------

static constexpr float ATTACK_TIME  = 0.015f;   // 15ms attack — eliminates click
static constexpr float RELEASE_TIME = 0.040f;    // 40ms release — smooth tail
static constexpr float MASTER_GAIN  = 0.25f;     // keep output comparable to other system audio

// ---------------------------------------------------------------------------
// AudioEngine
// ---------------------------------------------------------------------------

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

    // Request small buffer for low latency (~5.8ms at 44100 Hz)
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "128");

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
    stopMicCapture();
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
    // Precompute envelope rates
    v.attackRate = 1.0f / (ATTACK_TIME * static_cast<float>(sampleRate_));
    v.releaseRate = 1.0f / (RELEASE_TIME * static_cast<float>(sampleRate_));
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
        v->envStage = EnvStage::Attack;
        v->envLevel = 0.0f;
    }
}

void AudioEngine::stopVoice(int id, double when)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto* v = findVoice(id)) {
        v->stopTime = when;
        // Transition to release stage immediately
        if (v->envStage == EnvStage::Attack || v->envStage == EnvStage::Sustain) {
            v->envStage = EnvStage::Release;
        }
    }
}

// ---------------------------------------------------------------------------
// Microphone capture
// ---------------------------------------------------------------------------

bool AudioEngine::startMicCapture()
{
    if (micCapturing_) return true;
    if (!initialized_) return false;

    // Request small buffer for low-latency mic capture
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "128");

    SDL_AudioSpec spec;
    spec.format = SDL_AUDIO_F32;
    spec.channels = 1;
    spec.freq = sampleRate_;

    micStream_ = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_RECORDING,
        &spec, micCallback, this);

    if (!micStream_) {
        LOG_ERROR("Failed to open mic device: %s", SDL_GetError());
        return false;
    }

    SDL_ResumeAudioStreamDevice(micStream_);
    micCapturing_ = true;
    LOG_INFO("Microphone capture started");
    return true;
}

void AudioEngine::stopMicCapture()
{
    if (!micCapturing_) return;
    if (micStream_) {
        SDL_DestroyAudioStream(micStream_);
        micStream_ = nullptr;
    }
    micCapturing_ = false;
    LOG_INFO("Microphone capture stopped");
}

void AudioEngine::micCallback(void* userdata, SDL_AudioStream* stream,
                               int additional_amount, int /*total_amount*/)
{
    auto* engine = static_cast<AudioEngine*>(userdata);

    int avail = SDL_GetAudioStreamAvailable(stream);
    if (avail <= 0) return;

    int numSamples = avail / static_cast<int>(sizeof(float));
    float stackBuf[4096];
    float* buffer = (numSamples <= 4096) ? stackBuf : new float[numSamples];

    int got = SDL_GetAudioStreamData(stream, buffer, avail);
    if (got > 0) {
        int samplesGot = got / static_cast<int>(sizeof(float));
        {
            std::lock_guard<std::mutex> lock(engine->micMutex_);
            engine->micBuffer_.write(buffer, samplesGot);
        }
        // Write to playback FIFO (lock-free single-producer)
        int cap = static_cast<int>(engine->micPlayback_.size());
        uint64_t wp = engine->micPlaybackWritePos_.load(std::memory_order_relaxed);
        for (int i = 0; i < samplesGot; i++) {
            engine->micPlayback_[static_cast<int>((wp + i) % cap)] = buffer[i];
        }
        engine->micPlaybackWritePos_.store(wp + samplesGot, std::memory_order_release);
    }

    if (buffer != stackBuf) delete[] buffer;
}

// ---------------------------------------------------------------------------
// Output audio callback + synthesis
// ---------------------------------------------------------------------------

void AudioEngine::audioCallback(void* userdata, SDL_AudioStream* stream,
                                int additional_amount, int /*total_amount*/)
{
    auto* engine = static_cast<AudioEngine*>(userdata);
    int numSamples = additional_amount / static_cast<int>(sizeof(float));
    if (numSamples <= 0) return;

    float stackBuf[4096];
    float* buffer = (numSamples <= 4096) ? stackBuf : new float[numSamples];

    std::memset(buffer, 0, numSamples * sizeof(float));
    engine->generateSamples(buffer, numSamples);

    // Mix in mic playback if not muted
    if (!engine->micMuted_.load(std::memory_order_relaxed)) {
        float micGain = engine->micMonitorGain_.load(std::memory_order_relaxed);
        uint64_t wp = engine->micPlaybackWritePos_.load(std::memory_order_acquire);
        int cap = static_cast<int>(engine->micPlayback_.size());
        uint64_t available = wp - engine->micPlaybackReadPos_;

        // If too far behind (> ~15ms at 44100 Hz), snap to latest
        if (available > 660) {
            engine->micPlaybackReadPos_ = wp - numSamples;
            available = numSamples;
        }

        // Consume continuously from read position — no skipping
        int toRead = static_cast<int>(std::min(available, static_cast<uint64_t>(numSamples)));
        for (int i = 0; i < toRead; i++) {
            int idx = static_cast<int>((engine->micPlaybackReadPos_ + i) % cap);
            buffer[i] += engine->micPlayback_[idx] * micGain;
        }
        engine->micPlaybackReadPos_ += toRead;
    }

    // Write to output ring buffer for analysis (includes mic if unmuted)
    engine->outputBuffer_.write(buffer, numSamples);

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
        if (voice.envStage == EnvStage::Done) continue;

        float freq = voice.frequency;
        float gain = voice.gain;
        float phaseInc = freq / static_cast<float>(sampleRate_);

        for (int i = 0; i < numSamples; i++) {
            double t = baseTime + i * sampleDt;

            // Check start time
            if (t < voice.startTime) continue;

            // Envelope state machine
            switch (voice.envStage) {
                case EnvStage::Attack:
                    voice.envLevel += voice.attackRate;
                    if (voice.envLevel >= 1.0f) {
                        voice.envLevel = 1.0f;
                        voice.envStage = EnvStage::Sustain;
                    }
                    break;
                case EnvStage::Sustain:
                    // Check if stop was scheduled in the past
                    if (voice.stopTime >= 0 && t >= voice.stopTime) {
                        voice.envStage = EnvStage::Release;
                    }
                    break;
                case EnvStage::Release:
                    voice.envLevel -= voice.releaseRate;
                    if (voice.envLevel <= 0.0f) {
                        voice.envLevel = 0.0f;
                        voice.envStage = EnvStage::Done;
                        voice.active = false;
                        break;
                    }
                    break;
                case EnvStage::Done:
                case EnvStage::Idle:
                    break;
            }

            if (voice.envStage == EnvStage::Done) break;

            float sample = generateSample(voice.waveform, voice.phase, phaseInc);
            buffer[i] += sample * gain * voice.envLevel;

            voice.phase += phaseInc;
            if (voice.phase >= 1.0f) voice.phase -= 1.0f;
        }
    }

    // Soft limiter on the mix bus, then master gain
    for (int i = 0; i < numSamples; i++) {
        buffer[i] = softLimit(buffer[i]) * MASTER_GAIN;
    }

    // Clean up finished voices
    voices_.erase(
        std::remove_if(voices_.begin(), voices_.end(),
                       [](const Voice& v) {
                           return v.started && (v.envStage == EnvStage::Done || !v.active);
                       }),
        voices_.end());

    samplesGenerated_.fetch_add(static_cast<uint64_t>(numSamples), std::memory_order_relaxed);
}

} // namespace bro::audio
