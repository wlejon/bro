#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

struct SDL_AudioStream;

namespace bro::audio {

enum class Waveform : uint8_t { Sine, Square, Sawtooth, Triangle };

// Envelope state machine
enum class EnvStage : uint8_t { Idle, Attack, Sustain, Release, Done };

struct Voice {
    int id = 0;
    Waveform waveform = Waveform::Sine;
    float frequency = 440.0f;
    float gain = 1.0f;           // target amplitude from GainNode
    double startTime = -1.0;     // seconds, -1 = not started
    double stopTime = -1.0;      // seconds, -1 = not scheduled
    float phase = 0.0f;
    bool active = false;
    bool started = false;        // start() was called

    // ADSR envelope
    EnvStage envStage = EnvStage::Idle;
    float envLevel = 0.0f;       // current envelope amplitude (0..1)
    float attackRate = 0.0f;     // per-sample increment during attack
    float releaseRate = 0.0f;    // per-sample decrement during release
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
// Audio clip (immutable sample data) + playback instance
// ---------------------------------------------------------------------------

/// Immutable audio sample data. Created once, referenced by playback instances.
struct AudioClip {
    int id = 0;
    std::vector<float> samples;
};

/// A playing instance of an AudioClip. Supports looping, gain, and region control.
/// Suitable for game sounds (one-shot), music loops, audio clips, etc.
struct ClipPlayback {
    int id = 0;
    int clipId = 0;                     // references AudioClip::id
    int regionStart = 0;                // first sample (inclusive)
    int regionEnd = 0;                  // last sample (exclusive), 0 = full clip
    std::atomic<uint64_t> playPos{0};   // current position within region
    std::atomic<float> gain{1.0f};
    std::atomic<bool> playing{false};
    std::atomic<bool> looping{false};
    std::atomic<bool> active{true};     // false = finished/deleted
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

    void setMicMuted(bool muted) { micMuted_.store(muted, std::memory_order_relaxed); }
    bool isMicMuted() const { return micMuted_.load(std::memory_order_relaxed); }

    void setMicMonitorGain(float g) { micMonitorGain_.store(g, std::memory_order_relaxed); }
    float micMonitorGain() const { return micMonitorGain_.load(std::memory_order_relaxed); }

    // --- Recording ---

    void startRecording();
    void stopRecording();
    bool isRecording() const { return recording_.load(std::memory_order_relaxed); }
    const std::vector<float>& getRecordBuffer() const { return recordOutput_; }

    // --- Audio Clips ---

    /// Create a clip from raw sample data. Returns clip ID.
    int createClip(const float* samples, int numSamples);
    void deleteClip(int clipId);
    int getClipSampleCount(int clipId) const;
    /// Downsample clip to numBins min/max pairs for waveform display.
    void getClipWaveform(int clipId, float* outMinMax, int numBins) const;

    // --- Clip Playback ---

    /// Start playing a clip. Returns playback instance ID.
    int playClip(int clipId, float gain = 1.0f, bool loop = false);
    void stopPlayback(int instanceId);
    void setPlaybackGain(int instanceId, float gain);
    void setPlaybackLoop(int instanceId, bool loop);
    void setPlaybackRegion(int instanceId, int start, int end);
    void setPlaybackPlaying(int instanceId, bool playing);
    /// Returns normalized position (0..1) within the active region.
    float getPlaybackPosition(int instanceId) const;

private:
    static void audioCallback(void* userdata, SDL_AudioStream* stream,
                              int additional_amount, int total_amount);
    void generateSamples(float* buffer, int numSamples);

    static void micCallback(void* userdata, SDL_AudioStream* stream,
                            int additional_amount, int total_amount);

    Voice* findVoice(int id);
    AudioClip* findClip(int clipId) const;
    ClipPlayback* findPlayback(int instanceId) const;

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

    // Mic playback FIFO
    std::vector<float> micPlayback_ = std::vector<float>(4096, 0.0f);
    std::atomic<uint64_t> micPlaybackWritePos_{0};
    uint64_t micPlaybackReadPos_ = 0;

    std::atomic<bool> micMuted_{true};
    std::atomic<float> micMonitorGain_{0.5f};

    // Recording ring buffer
    static constexpr int RECORD_RING_SIZE = 44100 * 60; // 60 seconds
    std::vector<float> recordRing_ = std::vector<float>(RECORD_RING_SIZE, 0.0f);
    std::atomic<uint64_t> recordWritePos_{0};
    uint64_t recordStartPos_ = 0;
    std::atomic<bool> recording_{false};
    std::vector<float> recordOutput_;

    // Audio clips and playback instances
    std::vector<std::unique_ptr<AudioClip>> clips_;
    std::vector<std::unique_ptr<ClipPlayback>> playbacks_;
    mutable std::mutex clipMutex_;
    int nextClipId_ = 1;
    int nextPlaybackId_ = 1;
};

} // namespace bro::audio
