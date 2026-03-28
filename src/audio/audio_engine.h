#pragma once

#include <atomic>
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

private:
    static void audioCallback(void* userdata, SDL_AudioStream* stream,
                              int additional_amount, int total_amount);
    void generateSamples(float* buffer, int numSamples);

    Voice* findVoice(int id);

    SDL_AudioStream* stream_ = nullptr;
    std::vector<Voice> voices_;
    std::mutex mutex_;
    std::atomic<uint64_t> samplesGenerated_{0};
    int sampleRate_ = 44100;
    int nextVoiceId_ = 1;
    bool initialized_ = false;
};

} // namespace bro::audio
