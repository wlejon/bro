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

// Soft clipper: linear below threshold, smooth saturation above.
// No harmonic distortion in the linear region (unlike tanh which always distorts).
static inline float softLimit(float x)
{
    constexpr float thresh = 0.8f;
    if (x > thresh) {
        float over = x - thresh;
        return thresh + (1.0f - thresh) * (1.0f - std::exp(-over / (1.0f - thresh)));
    }
    if (x < -thresh) {
        float over = -x - thresh;
        return -(thresh + (1.0f - thresh) * (1.0f - std::exp(-over / (1.0f - thresh))));
    }
    return x;
}

// ---------------------------------------------------------------------------
// Compressor — output dynamics processing
// ---------------------------------------------------------------------------

void Compressor::init(int sampleRate)
{
    float sr = static_cast<float>(sampleRate);
    // Fast attack (~1ms) to catch transients
    attackCoeff = 1.0f - std::exp(-1.0f / (0.001f * sr));
    // Slow release (~100ms) to avoid pumping
    releaseCoeff = 1.0f - std::exp(-1.0f / (0.100f * sr));
    envelope = 0.0f;
}

void Compressor::process(float* buffer, int numSamples)
{
    for (int i = 0; i < numSamples; i++) {
        float absLevel = std::fabs(buffer[i]);

        // Envelope follower: fast attack, slow release
        if (absLevel > envelope)
            envelope += attackCoeff * (absLevel - envelope);
        else
            envelope += releaseCoeff * (absLevel - envelope);

        // Compute gain reduction
        float gain = 1.0f;
        if (envelope > threshold) {
            // Compress: target level = threshold + (excess / ratio)
            float target = threshold + (envelope - threshold) / ratio;
            gain = target / envelope;
        }

        buffer[i] *= gain;
    }
}

// ---------------------------------------------------------------------------
// BiquadFilter — Audio EQ Cookbook (Robert Bristow-Johnson)
// ---------------------------------------------------------------------------

void BiquadFilter::computeCoefficients(int sampleRate)
{
    float w0 = 2.0f * static_cast<float>(M_PI) * frequency / static_cast<float>(sampleRate);
    float sinW0 = std::sin(w0);
    float cosW0 = std::cos(w0);
    float alpha = sinW0 / (2.0f * Q);

    float a0 = 1.0f;

    switch (type) {
        case Type::Lowpass:
            b0 = (1.0f - cosW0) / 2.0f;
            b1 = 1.0f - cosW0;
            b2 = (1.0f - cosW0) / 2.0f;
            a0 = 1.0f + alpha;
            a1 = -2.0f * cosW0;
            a2 = 1.0f - alpha;
            break;
        case Type::Highpass:
            b0 = (1.0f + cosW0) / 2.0f;
            b1 = -(1.0f + cosW0);
            b2 = (1.0f + cosW0) / 2.0f;
            a0 = 1.0f + alpha;
            a1 = -2.0f * cosW0;
            a2 = 1.0f - alpha;
            break;
        case Type::Bandpass:
            b0 = alpha;
            b1 = 0.0f;
            b2 = -alpha;
            a0 = 1.0f + alpha;
            a1 = -2.0f * cosW0;
            a2 = 1.0f - alpha;
            break;
        case Type::Notch:
            b0 = 1.0f;
            b1 = -2.0f * cosW0;
            b2 = 1.0f;
            a0 = 1.0f + alpha;
            a1 = -2.0f * cosW0;
            a2 = 1.0f - alpha;
            break;
        case Type::Allpass:
            b0 = 1.0f - alpha;
            b1 = -2.0f * cosW0;
            b2 = 1.0f + alpha;
            a0 = 1.0f + alpha;
            a1 = -2.0f * cosW0;
            a2 = 1.0f - alpha;
            break;
        case Type::Peaking: {
            float A = std::pow(10.0f, gainDB / 40.0f);
            b0 = 1.0f + alpha * A;
            b1 = -2.0f * cosW0;
            b2 = 1.0f - alpha * A;
            a0 = 1.0f + alpha / A;
            a1 = -2.0f * cosW0;
            a2 = 1.0f - alpha / A;
            break;
        }
        case Type::Lowshelf: {
            float A = std::pow(10.0f, gainDB / 40.0f);
            float twoSqrtAAlpha = 2.0f * std::sqrt(A) * alpha;
            b0 = A * ((A + 1.0f) - (A - 1.0f) * cosW0 + twoSqrtAAlpha);
            b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosW0);
            b2 = A * ((A + 1.0f) - (A - 1.0f) * cosW0 - twoSqrtAAlpha);
            a0 = (A + 1.0f) + (A - 1.0f) * cosW0 + twoSqrtAAlpha;
            a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cosW0);
            a2 = (A + 1.0f) + (A - 1.0f) * cosW0 - twoSqrtAAlpha;
            break;
        }
        case Type::Highshelf: {
            float A = std::pow(10.0f, gainDB / 40.0f);
            float twoSqrtAAlpha = 2.0f * std::sqrt(A) * alpha;
            b0 = A * ((A + 1.0f) + (A - 1.0f) * cosW0 + twoSqrtAAlpha);
            b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosW0);
            b2 = A * ((A + 1.0f) + (A - 1.0f) * cosW0 - twoSqrtAAlpha);
            a0 = (A + 1.0f) - (A - 1.0f) * cosW0 + twoSqrtAAlpha;
            a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cosW0);
            a2 = (A + 1.0f) - (A - 1.0f) * cosW0 - twoSqrtAAlpha;
            break;
        }
    }

    // Normalize by a0
    b0 /= a0;
    b1 /= a0;
    b2 /= a0;
    a1 /= a0;
    a2 /= a0;
}

// ---------------------------------------------------------------------------
// DelayEffect
// ---------------------------------------------------------------------------

void DelayEffect::init(int maxDelaySamples)
{
    buffer.assign(maxDelaySamples, 0.0f);
    writePos = 0;
}

void DelayEffect::process(float* buf, int numSamples)
{
    int bufSize = static_cast<int>(buffer.size());
    if (bufSize == 0 || delaySamples <= 0) return;

    for (int i = 0; i < numSamples; i++) {
        int readPos = (writePos - delaySamples + bufSize) % bufSize;
        float delayed = buffer[readPos];
        float dry = buf[i];
        buf[i] = dry * (1.0f - mix) + delayed * mix;
        buffer[writePos] = dry + delayed * feedback;
        writePos = (writePos + 1) % bufSize;
    }
}

// ---------------------------------------------------------------------------
// Envelope constants
// ---------------------------------------------------------------------------

static constexpr float DEFAULT_ATTACK  = 0.01f;
static constexpr float DEFAULT_DECAY   = 0.1f;
static constexpr float DEFAULT_SUSTAIN = 1.0f;
static constexpr float DEFAULT_RELEASE = 0.04f;

// Fixed per-voice amplitude. 10 simultaneous voices sum to ~1.0.
// Volume control is post-mix via masterGain_, never per-voice.
static constexpr float VOICE_AMPLITUDE = 0.1f;

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

    // Request small buffer for low latency (~2.9ms at 44100 Hz)
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

    // Initialize delay buffer: 2 seconds max
    delay_.init(sampleRate_ * 2);
    compressor_.init(sampleRate_);

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
    float sr = static_cast<float>(sampleRate_);
    v.attackRate = 1.0f / (DEFAULT_ATTACK * sr);
    // Exponential coefficients: 3 time constants ≈ 95% of target in `seconds`
    v.decayCoeff = std::exp(-3.0f / (DEFAULT_DECAY * sr));
    v.sustainLevel = DEFAULT_SUSTAIN;
    v.releaseCoeff = std::exp(-3.0f / (DEFAULT_RELEASE * sr));
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

void AudioEngine::setMasterGain(float gain)
{
    masterGain_.store(std::clamp(gain, 0.0f, 2.0f), std::memory_order_relaxed);
}

void AudioEngine::setAttackTime(int id, float seconds)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto* v = findVoice(id))
        v->attackRate = seconds > 0.0001f ? 1.0f / (seconds * static_cast<float>(sampleRate_)) : 1.0f;
}

void AudioEngine::setDecayTime(int id, float seconds)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto* v = findVoice(id))
        v->decayCoeff = seconds > 0.0001f
            ? std::exp(-3.0f / (seconds * static_cast<float>(sampleRate_)))
            : 0.0f;
}

void AudioEngine::setSustainLevel(int id, float level)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto* v = findVoice(id))
        v->sustainLevel = std::clamp(level, 0.0f, 1.0f);
}

void AudioEngine::setReleaseTime(int id, float seconds)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto* v = findVoice(id))
        v->releaseCoeff = seconds > 0.0001f
            ? std::exp(-3.0f / (seconds * static_cast<float>(sampleRate_)))
            : 0.0f;
}

// ---------------------------------------------------------------------------
// Filter control
// ---------------------------------------------------------------------------

int AudioEngine::allocateFilterSlot()
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (int i = 0; i < MAX_FILTERS; i++) {
        if (!filters_[i].enabled) {
            filters_[i] = BiquadFilter{};
            return i;
        }
    }
    return -1;
}

void AudioEngine::releaseFilterSlot(int slot)
{
    if (slot < 0 || slot >= MAX_FILTERS) return;
    std::lock_guard<std::mutex> lock(mutex_);
    filters_[slot].enabled = false;
    filters_[slot].reset();
}

void AudioEngine::setFilterEnabled(int slot, bool enabled)
{
    if (slot < 0 || slot >= MAX_FILTERS) return;
    std::lock_guard<std::mutex> lock(mutex_);
    filters_[slot].enabled = enabled;
    if (enabled) filters_[slot].computeCoefficients(sampleRate_);
}

void AudioEngine::setFilterType(int slot, BiquadFilter::Type type)
{
    if (slot < 0 || slot >= MAX_FILTERS) return;
    std::lock_guard<std::mutex> lock(mutex_);
    filters_[slot].type = type;
    filters_[slot].reset();
    if (filters_[slot].enabled) filters_[slot].computeCoefficients(sampleRate_);
}

void AudioEngine::setFilterFrequency(int slot, float freq)
{
    if (slot < 0 || slot >= MAX_FILTERS) return;
    std::lock_guard<std::mutex> lock(mutex_);
    filters_[slot].frequency = std::clamp(freq, 20.0f, 20000.0f);
    if (filters_[slot].enabled) filters_[slot].computeCoefficients(sampleRate_);
}

void AudioEngine::setFilterQ(int slot, float q)
{
    if (slot < 0 || slot >= MAX_FILTERS) return;
    std::lock_guard<std::mutex> lock(mutex_);
    filters_[slot].Q = std::clamp(q, 0.1f, 30.0f);
    if (filters_[slot].enabled) filters_[slot].computeCoefficients(sampleRate_);
}

void AudioEngine::setFilterGain(int slot, float gainDB)
{
    if (slot < 0 || slot >= MAX_FILTERS) return;
    std::lock_guard<std::mutex> lock(mutex_);
    filters_[slot].gainDB = std::clamp(gainDB, -40.0f, 40.0f);
    if (filters_[slot].enabled) filters_[slot].computeCoefficients(sampleRate_);
}

// ---------------------------------------------------------------------------
// Delay control
// ---------------------------------------------------------------------------

void AudioEngine::setDelayEnabled(bool enabled)
{
    std::lock_guard<std::mutex> lock(mutex_);
    delay_.enabled = enabled;
}

void AudioEngine::setDelayTime(float seconds)
{
    std::lock_guard<std::mutex> lock(mutex_);
    int maxSamples = static_cast<int>(delay_.buffer.size());
    delay_.delaySamples = std::clamp(static_cast<int>(seconds * sampleRate_), 1, maxSamples - 1);
}

void AudioEngine::setDelayFeedback(float fb)
{
    std::lock_guard<std::mutex> lock(mutex_);
    delay_.feedback = std::clamp(fb, 0.0f, 0.95f);
}

void AudioEngine::setDelayMix(float mix)
{
    std::lock_guard<std::mutex> lock(mutex_);
    delay_.mix = std::clamp(mix, 0.0f, 1.0f);
}

// ---------------------------------------------------------------------------
// Voice start/stop
// ---------------------------------------------------------------------------

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
        if (v->envStage == EnvStage::Attack || v->envStage == EnvStage::Decay
            || v->envStage == EnvStage::Sustain) {
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
// Recording
// ---------------------------------------------------------------------------

void AudioEngine::startRecording()
{
    recordStartPos_ = recordWritePos_.load(std::memory_order_relaxed);
    recording_.store(true, std::memory_order_release);
    LOG_INFO("Recording started");
}

void AudioEngine::stopRecording()
{
    recording_.store(false, std::memory_order_release);

    uint64_t endPos = recordWritePos_.load(std::memory_order_acquire);
    uint64_t startPos = recordStartPos_;
    int count = static_cast<int>(endPos - startPos);
    if (count <= 0) {
        recordOutput_.clear();
        return;
    }
    // Cap at ring size
    if (count > RECORD_RING_SIZE) {
        startPos = endPos - RECORD_RING_SIZE;
        count = RECORD_RING_SIZE;
    }
    recordOutput_.resize(count);
    for (int i = 0; i < count; i++) {
        recordOutput_[i] = recordRing_[static_cast<int>((startPos + i) % RECORD_RING_SIZE)];
    }
    LOG_INFO("Recording stopped: %d samples (%.1f sec)", count,
             static_cast<float>(count) / static_cast<float>(sampleRate_));
}

// ---------------------------------------------------------------------------
// Audio Clips
// ---------------------------------------------------------------------------

AudioClip* AudioEngine::findClip(int clipId) const
{
    for (auto& c : clips_) {
        if (c->id == clipId) return c.get();
    }
    return nullptr;
}

ClipPlayback* AudioEngine::findPlayback(int instanceId) const
{
    for (auto& pb : playbacks_) {
        if (pb->id == instanceId && pb->active.load(std::memory_order_relaxed)) return pb.get();
    }
    return nullptr;
}

int AudioEngine::createClip(const float* samples, int numSamples)
{
    if (numSamples <= 0 || !samples) return -1;

    auto clip = std::make_unique<AudioClip>();
    std::lock_guard<std::mutex> lock(clipMutex_);
    clip->id = nextClipId_++;
    clip->samples.assign(samples, samples + numSamples);

    int id = clip->id;
    clips_.push_back(std::move(clip));
    LOG_INFO("AudioClip %d created: %d samples (%.1f sec)", id, numSamples,
             static_cast<float>(numSamples) / static_cast<float>(sampleRate_));
    return id;
}

void AudioEngine::deleteClip(int clipId)
{
    std::lock_guard<std::mutex> lock(clipMutex_);
    // Stop and remove all playback instances referencing this clip
    for (auto& pb : playbacks_) {
        if (pb->clipId == clipId) {
            pb->playing.store(false, std::memory_order_relaxed);
            pb->active.store(false, std::memory_order_relaxed);
        }
    }
    playbacks_.erase(
        std::remove_if(playbacks_.begin(), playbacks_.end(),
                       [](const std::unique_ptr<ClipPlayback>& pb) {
                           return !pb->active.load(std::memory_order_relaxed);
                       }),
        playbacks_.end());
    clips_.erase(
        std::remove_if(clips_.begin(), clips_.end(),
                       [clipId](const std::unique_ptr<AudioClip>& c) { return c->id == clipId; }),
        clips_.end());
}

int AudioEngine::getClipSampleCount(int clipId) const
{
    std::lock_guard<std::mutex> lock(clipMutex_);
    if (auto* c = findClip(clipId)) return static_cast<int>(c->samples.size());
    return 0;
}

void AudioEngine::getClipWaveform(int clipId, float* outMinMax, int numBins) const
{
    std::lock_guard<std::mutex> lock(clipMutex_);
    auto* clip = findClip(clipId);
    if (!clip || clip->samples.empty()) {
        for (int i = 0; i < numBins * 2; i++) outMinMax[i] = 0.0f;
        return;
    }

    int totalSamples = static_cast<int>(clip->samples.size());
    float samplesPerBin = static_cast<float>(totalSamples) / static_cast<float>(numBins);

    for (int b = 0; b < numBins; b++) {
        int startIdx = static_cast<int>(b * samplesPerBin);
        int endIdx = static_cast<int>((b + 1) * samplesPerBin);
        endIdx = std::min(endIdx, totalSamples);

        float minVal = 1.0f, maxVal = -1.0f;
        for (int i = startIdx; i < endIdx; i++) {
            float s = clip->samples[i];
            if (s < minVal) minVal = s;
            if (s > maxVal) maxVal = s;
        }
        outMinMax[b * 2] = minVal;
        outMinMax[b * 2 + 1] = maxVal;
    }
}

// ---------------------------------------------------------------------------
// Clip Playback
// ---------------------------------------------------------------------------

int AudioEngine::playClip(int clipId, float gain, bool loop)
{
    std::lock_guard<std::mutex> lock(clipMutex_);
    if (!findClip(clipId)) return -1;

    auto pb = std::make_unique<ClipPlayback>();
    pb->id = nextPlaybackId_++;
    pb->clipId = clipId;
    pb->gain.store(gain, std::memory_order_relaxed);
    pb->looping.store(loop, std::memory_order_relaxed);
    pb->playing.store(true, std::memory_order_relaxed);
    pb->active.store(true, std::memory_order_relaxed);
    pb->playPos.store(0, std::memory_order_relaxed);
    pb->regionStart = 0;
    pb->regionEnd = 0; // 0 = full clip

    int id = pb->id;
    playbacks_.push_back(std::move(pb));
    return id;
}

void AudioEngine::stopPlayback(int instanceId)
{
    std::lock_guard<std::mutex> lock(clipMutex_);
    if (auto* pb = findPlayback(instanceId)) {
        pb->playing.store(false, std::memory_order_relaxed);
        pb->active.store(false, std::memory_order_relaxed);
    }
    // Clean up inactive
    playbacks_.erase(
        std::remove_if(playbacks_.begin(), playbacks_.end(),
                       [](const std::unique_ptr<ClipPlayback>& pb) {
                           return !pb->active.load(std::memory_order_relaxed);
                       }),
        playbacks_.end());
}

void AudioEngine::setPlaybackGain(int instanceId, float gain)
{
    std::lock_guard<std::mutex> lock(clipMutex_);
    if (auto* pb = findPlayback(instanceId)) {
        pb->gain.store(gain, std::memory_order_relaxed);
    }
}

void AudioEngine::setPlaybackLoop(int instanceId, bool loop)
{
    std::lock_guard<std::mutex> lock(clipMutex_);
    if (auto* pb = findPlayback(instanceId)) {
        pb->looping.store(loop, std::memory_order_relaxed);
    }
}

void AudioEngine::setPlaybackPlaying(int instanceId, bool playing)
{
    std::lock_guard<std::mutex> lock(clipMutex_);
    if (auto* pb = findPlayback(instanceId)) {
        if (playing && !pb->playing.load(std::memory_order_relaxed)) {
            pb->playPos.store(0, std::memory_order_relaxed);
        }
        pb->playing.store(playing, std::memory_order_relaxed);
    }
}

void AudioEngine::setPlaybackRegion(int instanceId, int start, int end)
{
    std::lock_guard<std::mutex> lock(clipMutex_);
    if (auto* pb = findPlayback(instanceId)) {
        auto* clip = findClip(pb->clipId);
        if (!clip) return;
        int maxLen = static_cast<int>(clip->samples.size());
        pb->regionStart = std::clamp(start, 0, maxLen);
        pb->regionEnd = std::clamp(end, pb->regionStart, maxLen);
        pb->playPos.store(0, std::memory_order_relaxed);
    }
}

float AudioEngine::getPlaybackPosition(int instanceId) const
{
    std::lock_guard<std::mutex> lock(clipMutex_);
    auto* pb = findPlayback(instanceId);
    if (!pb) return 0.0f;
    auto* clip = findClip(pb->clipId);
    if (!clip) return 0.0f;

    int end = pb->regionEnd > 0 ? pb->regionEnd : static_cast<int>(clip->samples.size());
    int len = end - pb->regionStart;
    if (len <= 0) return 0.0f;
    uint64_t pos = pb->playPos.load(std::memory_order_relaxed);
    return static_cast<float>(pos % len) / static_cast<float>(len);
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

        if (available > 660) {
            engine->micPlaybackReadPos_ = wp - numSamples;
            available = numSamples;
        }

        int toRead = static_cast<int>(std::min(available, static_cast<uint64_t>(numSamples)));
        for (int i = 0; i < toRead; i++) {
            int idx = static_cast<int>((engine->micPlaybackReadPos_ + i) % cap);
            buffer[i] += engine->micPlayback_[idx] * micGain;
        }
        engine->micPlaybackReadPos_ += toRead;
    }

    // Record tap: capture synth+mic BEFORE adding loops (avoids feedback)
    if (engine->recording_.load(std::memory_order_relaxed)) {
        uint64_t wp = engine->recordWritePos_.load(std::memory_order_relaxed);
        for (int i = 0; i < numSamples; i++) {
            engine->recordRing_[static_cast<int>((wp + i) % RECORD_RING_SIZE)] = buffer[i];
        }
        engine->recordWritePos_.store(wp + numSamples, std::memory_order_release);
    }

    // Mix active clip playback instances into output
    {
        std::lock_guard<std::mutex> lock(engine->clipMutex_);
        for (auto& pb : engine->playbacks_) {
            if (!pb->active.load(std::memory_order_relaxed)) continue;
            if (!pb->playing.load(std::memory_order_relaxed)) continue;

            auto* clip = engine->findClip(pb->clipId);
            if (!clip) continue;

            int start = pb->regionStart;
            int end = pb->regionEnd > 0 ? pb->regionEnd : static_cast<int>(clip->samples.size());
            int len = end - start;
            if (len <= 0) continue;

            uint64_t pos = pb->playPos.load(std::memory_order_relaxed);
            float g = pb->gain.load(std::memory_order_relaxed);
            bool looping = pb->looping.load(std::memory_order_relaxed);

            for (int i = 0; i < numSamples; i++) {
                int sampleIdx = static_cast<int>(pos % len);
                buffer[i] += clip->samples[start + sampleIdx] * g;
                pos++;
                if (!looping && static_cast<int>(pos) >= len) {
                    pb->playing.store(false, std::memory_order_relaxed);
                    pb->active.store(false, std::memory_order_relaxed);
                    break;
                }
            }
            pb->playPos.store(pos, std::memory_order_relaxed);
        }
    }

    // Apply filter chain (post-mix, pre-limiter)
    {
        std::lock_guard<std::mutex> lock(engine->mutex_);
        for (int f = 0; f < AudioEngine::MAX_FILTERS; f++) {
            if (!engine->filters_[f].enabled) continue;
            for (int i = 0; i < numSamples; i++) {
                buffer[i] = engine->filters_[f].process(buffer[i]);
            }
        }

        // Apply delay effect
        if (engine->delay_.enabled) {
            engine->delay_.process(buffer, numSamples);
        }
    }

    // Compressor — keeps polyphony clean without per-voice gain hacking
    engine->compressor_.process(buffer, numSamples);

    // Soft limiter (safety net), then user-controlled master gain
    float mg = engine->masterGain_.load(std::memory_order_relaxed);
    for (int i = 0; i < numSamples; i++) {
        buffer[i] = softLimit(buffer[i]) * mg;
    }

    // Write to output ring buffer for analysis
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
        float gain = VOICE_AMPLITUDE * voice.gain; // voice.gain is per-voice (velocity etc)
        float phaseInc = freq / static_cast<float>(sampleRate_);

        for (int i = 0; i < numSamples; i++) {
            double t = baseTime + i * sampleDt;

            if (t < voice.startTime) continue;

            switch (voice.envStage) {
                case EnvStage::Attack:
                    voice.envLevel += voice.attackRate;
                    if (voice.envLevel >= 1.0f) {
                        voice.envLevel = 1.0f;
                        voice.envStage = EnvStage::Decay;
                    }
                    break;
                case EnvStage::Decay:
                    // Exponential approach toward sustainLevel
                    voice.envLevel = voice.sustainLevel
                        + (voice.envLevel - voice.sustainLevel) * voice.decayCoeff;
                    if (voice.envLevel - voice.sustainLevel < 0.001f) {
                        voice.envLevel = voice.sustainLevel;
                        voice.envStage = EnvStage::Sustain;
                    }
                    break;
                case EnvStage::Sustain:
                    voice.envLevel = voice.sustainLevel;
                    if (voice.stopTime >= 0 && t >= voice.stopTime) {
                        voice.envStage = EnvStage::Release;
                    }
                    break;
                case EnvStage::Release:
                    // Exponential decay toward zero — no click
                    voice.envLevel *= voice.releaseCoeff;
                    if (voice.envLevel < 0.0001f) {
                        voice.envLevel = 0.0f;
                        voice.envStage = EnvStage::Done;
                        voice.active = false;
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

    // NOTE: soft limiter moved to audioCallback to apply once after all mixing

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
