#pragma once

// bro::audio is now a thin alias layer over the broaudio library.
// All types and the engine class are re-exported into the bro::audio namespace
// so that existing code (JS bindings, engine.cpp) continues to compile unchanged.

#include <broaudio/engine.h>
#include <broaudio/dsp/fft.h>

namespace bro::audio {

// Re-export broaudio types into bro::audio
using broaudio::Waveform;
using broaudio::EnvStage;
using broaudio::Voice;
using broaudio::AudioClip;
using broaudio::ClipPlayback;
using broaudio::Compressor;
using broaudio::BiquadFilter;
using broaudio::DelayEffect;
using broaudio::FilterParams;
using broaudio::DelayParams;
using broaudio::AnalysisBuffer;

// Keep the old name for compatibility
using RingBuffer = broaudio::AnalysisBuffer;

// Re-export engine as AudioEngine
using AudioEngine = broaudio::Engine;

// Re-export FFT
using broaudio::fft;

} // namespace bro::audio
