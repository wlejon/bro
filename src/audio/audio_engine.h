#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <vector>

struct SDL_AudioStream;

namespace bro::audio {

enum class Waveform : uint8_t { Sine, Square, Sawtooth, Triangle };

struct Voice {
    int id = 0;
    Waveform waveform = Waveform::Sine;
    float frequency = 440.0f;
    float gain = 1.0f;           // from connected GainNode
    double startTime = -1.0;     // seconds, -1 = not started
    double stopTime = -1.0;      // seconds, -1 = not scheduled
    float phase = 0.0f;
    bool active = false;
    bool started = false;        // start() was called
};

// ---------------------------------------------------------------------------
// Ring buffer for audio analysis
// ---------------------------------------------------------------------------

class RingBuffer {
public:
    explicit RingBuffer(int capacity = 8192)
        : data_(capacity, 0.0f), capacity_(capacity) {}

    void write(const float* src, int count) {
        for (int i = 0; i < count; i++) {
            data_[writePos_ % capacity_] = src[i];
            writePos_++;
        }
    }

    /// Copy the most recent `count` samples into `dst`.
    void readLatest(float* dst, int count) const {
        int start = static_cast<int>(writePos_) - count;
        if (start < 0) start = 0;
        for (int i = 0; i < count; i++) {
            dst[i] = data_[(start + i) % capacity_];
        }
    }

    int capacity() const { return capacity_; }

private:
    std::vector<float> data_;
    int capacity_;
    uint64_t writePos_ = 0;
};

// ---------------------------------------------------------------------------
// FFT utility (radix-2 Cooley-Tukey)
// ---------------------------------------------------------------------------

void fft(float* real, float* imag, int n);

// ---------------------------------------------------------------------------
// AudioEngine
// ---------------------------------------------------------------------------

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool init();
    void shutdown();

    /// Current audio time in seconds (based on samples generated).
    double currentTime() const;

    /// Sample rate.
    int sampleRate() const { return sampleRate_; }

    /// Create a new voice, returns its ID.
    int createVoice();

    /// Remove a voice.
    void removeVoice(int id);

    /// Set voice parameters (thread-safe).
    void setWaveform(int id, Waveform wf);
    void setFrequency(int id, float freq);
    void setGain(int id, float gain);
    void startVoice(int id, double when);
    void stopVoice(int id, double when);

    // --- Analysis ---

    /// Get the output ring buffer (mix tap).
    RingBuffer& outputBuffer() { return outputBuffer_; }
    const RingBuffer& outputBuffer() const { return outputBuffer_; }

    /// Get the mic input ring buffer.
    RingBuffer& micBuffer() { return micBuffer_; }
    const RingBuffer& micBuffer() const { return micBuffer_; }

    // --- Microphone capture ---

    bool startMicCapture();
    void stopMicCapture();
    bool isMicCapturing() const { return micCapturing_; }

private:
    static void audioCallback(void* userdata, SDL_AudioStream* stream,
                              int additional_amount, int total_amount);
    void generateSamples(float* buffer, int numSamples);

    static void micCallback(void* userdata, SDL_AudioStream* stream,
                            int additional_amount, int total_amount);

    Voice* findVoice(int id);

    SDL_AudioStream* stream_ = nullptr;
    SDL_AudioStream* micStream_ = nullptr;
    std::vector<Voice> voices_;
    std::mutex mutex_;
    std::atomic<uint64_t> samplesGenerated_{0};
    int sampleRate_ = 44100;
    int nextVoiceId_ = 1;
    bool initialized_ = false;
    bool micCapturing_ = false;

    RingBuffer outputBuffer_{16384};
    RingBuffer micBuffer_{16384};
    std::mutex micMutex_;
};

} // namespace bro::audio
