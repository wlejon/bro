#pragma once

#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "engine/engine.h"
#include "util/log.h"

#include <broaudio/dsp/fft.h>
#include <broaudio/synth/wavetable.h>
#include <broaudio/dsp/resampler.h>
#include <broaudio/io/audio_file.h>
#include <broaudio/node/audio_node.h>
#include <broaudio/engine.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace bro::bronze_host {

inline constexpr uint32_t kHostPeriodicWaveTag = 0x50574156u;  // 'PWAV'

// ---------------------------------------------------------------------------
// Structs & Handle Cells
// ---------------------------------------------------------------------------

struct HostAudioContext {
    uint32_t tag = kHostAudioContextTag;
};

enum class AudioNodeType : uint8_t {
    Destination = 0,
    Gain,
    Oscillator,
    BiquadFilter,
    Analyser,
    BufferSource,
    Panner,
    StereoPanner,
    Delay,
    DynamicsCompressor,
    WaveShaper,
    Convolver,
    ChannelSplitter,
    ChannelMerger,
    Generic,
};

struct HostAudioNode {
    uint32_t tag = kHostAudioNodeTag;
    AudioNodeType nodeType = AudioNodeType::Generic;
};

enum class AudioParamTarget : uint8_t {
    Generic = 0,
    Gain,
    VoiceFrequency,
    VoiceDetune,
    VoicePan,
    FilterFrequency,
    FilterQ,
    FilterGain,
    PlaybackRate,
    PlaybackDetune,
    DelayTime,
    Pan,
    PannerPositionX,
    PannerPositionY,
    PannerPositionZ,
    PannerOrientationX,
    PannerOrientationY,
    PannerOrientationZ,
    CompressorThreshold,
    CompressorKnee,
    CompressorRatio,
    CompressorAttack,
    CompressorRelease,
};

struct HostAudioParam {
    uint32_t tag = kHostAudioParamTag;
    AudioParamTarget target = AudioParamTarget::Generic;
    int targetId = -1;
    float value = 1.0f;
    float defaultValue = 1.0f;
    float minValue = -3.402823466e+38f;
    float maxValue = 3.402823466e+38f;
};

struct HostGainNode {
    HostAudioNode base;
};

struct HostOscillatorNode {
    HostAudioNode base;
    int voiceId = -1;
    std::string type = "sine";
    bool started = false;
    bool stopped = false;
};

struct HostPeriodicWave {
    uint32_t tag = kHostPeriodicWaveTag;
    std::vector<float> real;
    std::vector<float> imag;
    bool disableNormalization = false;
    std::shared_ptr<broaudio::WavetableBank> wavetable;
};

struct HostBiquadFilterNode {
    HostAudioNode base;
    int slot = -1;
    std::string type = "lowpass";
};

struct HostAnalyserNode {
    HostAudioNode base;
    int fftSize = 2048;
    float minDecibels = -100.0f;
    float maxDecibels = -30.0f;
    float smoothingTimeConstant = 0.8f;
    std::vector<float> smoothedMagnitudes;
};

struct HostAudioBuffer {
    uint32_t tag = kHostAudioBufferTag;
    int numberOfChannels = 1;
    int length = 0;
    int sampleRate = 44100;
    std::vector<std::vector<float>> channels;
};

struct HostAudioBufferSourceNode {
    HostAudioNode base;
    HostAudioBuffer* buffer = nullptr;
    bool loop = false;
    double loopStart = 0.0;
    double loopEnd = 0.0;
    int clipId = -1;
    int playbackId = -1;
    bool started = false;
    bool stopped = false;
};

struct HostPannerNode {
    HostAudioNode base;
    std::string panningModel = "equalpower";
    std::string distanceModel = "inverse";
    float refDistance = 1.0f;
    float maxDistance = 10000.0f;
    float rolloffFactor = 1.0f;
    float coneInnerAngle = 360.0f;
    float coneOuterAngle = 360.0f;
    float coneOuterGain = 0.0f;
    float posX = 0.0f, posY = 0.0f, posZ = 0.0f;
    float orientX = 1.0f, orientY = 0.0f, orientZ = 0.0f;
};

struct HostStereoPannerNode {
    HostAudioNode base;
    float pan = 0.0f;
};

struct HostDelayNode {
    HostAudioNode base;
    double maxDelayTime = 1.0;
};

struct HostDynamicsCompressorNode {
    HostAudioNode base;
    float reduction = 0.0f;
};

struct HostWaveShaperNode {
    HostAudioNode base;
    std::vector<float> curve;
    std::string oversample = "none";
};

struct HostConvolverNode {
    HostAudioNode base;
    HostAudioBuffer* buffer = nullptr;
    bool normalize = true;
};

struct HostChannelSplitterNode {
    HostAudioNode base;
    int numberOfOutputs = 6;
};

struct HostChannelMergerNode {
    HostAudioNode base;
    int numberOfInputs = 6;
};

// ---------------------------------------------------------------------------
// HostClass Declarations (extern)
// ---------------------------------------------------------------------------

extern HostClass g_audioNodeClass;
extern HostClass g_audioParamClass;
extern HostClass g_audioContextClass;
extern HostClass g_audioBufferClass;
extern HostClass g_gainNodeClass;
extern HostClass g_oscillatorNodeClass;
extern HostClass g_periodicWaveClass;
extern HostClass g_biquadFilterNodeClass;
extern HostClass g_analyserNodeClass;
extern HostClass g_audioBufferSourceNodeClass;
extern HostClass g_pannerNodeClass;
extern HostClass g_stereoPannerNodeClass;
extern HostClass g_delayNodeClass;
extern HostClass g_dynamicsCompressorNodeClass;
extern HostClass g_waveShaperNodeClass;
extern HostClass g_convolverNodeClass;
extern HostClass g_channelSplitterNodeClass;
extern HostClass g_channelMergerNodeClass;

// ---------------------------------------------------------------------------
// Destructors (Finalizers)
// ---------------------------------------------------------------------------

void hostAudioContextDtor(void* p);
void hostAudioNodeDtor(void* p);
void hostAudioParamDtor(void* p);
void hostAudioBufferDtor(void* p);
void hostGainDtor(void* p);
void hostOscillatorDtor(void* p);
void hostPeriodicWaveDtor(void* p);
void hostBiquadFilterDtor(void* p);
void hostAnalyserDtor(void* p);
void hostAudioBufferSourceDtor(void* p);
void hostPannerDtor(void* p);
void hostStereoPannerDtor(void* p);
void hostDelayDtor(void* p);
void hostDynamicsCompressorDtor(void* p);
void hostWaveShaperDtor(void* p);
void hostConvolverDtor(void* p);
void hostChannelSplitterDtor(void* p);
void hostChannelMergerDtor(void* p);

// ---------------------------------------------------------------------------
// Unwrap Helpers
// ---------------------------------------------------------------------------

HostAudioContext* hostAudioContextOf(Value v);
HostAudioNode* hostAudioNodeOf(Value v);
HostAudioParam* hostAudioParamOf(Value v);
HostAudioBuffer* hostAudioBufferOf(Value v);
HostPeriodicWave* hostPeriodicWaveOf(Value v);

template <typename T>
T* nodeOfKind(Value v, AudioNodeType kind) {
    HostAudioNode* n = hostAudioNodeOf(v);
    if (!n || n->nodeType != kind) return nullptr;
    return reinterpret_cast<T*>(n);
}

inline HostOscillatorNode* oscOf(Value v) {
    return nodeOfKind<HostOscillatorNode>(v, AudioNodeType::Oscillator);
}
inline HostBiquadFilterNode* filterOf(Value v) {
    return nodeOfKind<HostBiquadFilterNode>(v, AudioNodeType::BiquadFilter);
}
inline HostAnalyserNode* analyserOf(Value v) {
    return nodeOfKind<HostAnalyserNode>(v, AudioNodeType::Analyser);
}
inline HostAudioBufferSourceNode* bufSrcOf(Value v) {
    return nodeOfKind<HostAudioBufferSourceNode>(v, AudioNodeType::BufferSource);
}
inline HostPannerNode* pannerOf(Value v) {
    return nodeOfKind<HostPannerNode>(v, AudioNodeType::Panner);
}
inline HostStereoPannerNode* stereoPannerOf(Value v) {
    return nodeOfKind<HostStereoPannerNode>(v, AudioNodeType::StereoPanner);
}
inline HostDelayNode* delayOf(Value v) {
    return nodeOfKind<HostDelayNode>(v, AudioNodeType::Delay);
}
inline HostDynamicsCompressorNode* compressorOf(Value v) {
    return nodeOfKind<HostDynamicsCompressorNode>(v, AudioNodeType::DynamicsCompressor);
}
inline HostWaveShaperNode* waveShaperOf(Value v) {
    return nodeOfKind<HostWaveShaperNode>(v, AudioNodeType::WaveShaper);
}
inline HostConvolverNode* convolverOf(Value v) {
    return nodeOfKind<HostConvolverNode>(v, AudioNodeType::Convolver);
}
inline HostChannelSplitterNode* channelSplitterOf(Value v) {
    return nodeOfKind<HostChannelSplitterNode>(v, AudioNodeType::ChannelSplitter);
}
inline HostChannelMergerNode* channelMergerOf(Value v) {
    return nodeOfKind<HostChannelMergerNode>(v, AudioNodeType::ChannelMerger);
}

// ---------------------------------------------------------------------------
// Helpers & Node Value Creators
// ---------------------------------------------------------------------------

void syncAudioParamValue(HostAudioParam* p, float val);
Value makeAudioParamValue(AudioParamTarget target, int targetId,
                          float initialVal, float minVal, float maxVal, float defaultVal);

// Core node creators & decorators (host_audio_core.cpp)
void decorateAudioNodeProto(ObjectBuilder& b);
void decorateAudioParamProto(ObjectBuilder& b);
void decorateAudioBufferProto(ObjectBuilder& b);
void decorateAudioContextProto(ObjectBuilder& b);
Value makeDestinationNodeValue();
Value makeListenerValue();
Value makeAudioBufferValue(int channels, int length, int sampleRate);
Value makeAudioContextValue();

// Nodes creators & decorators (host_audio_nodes.cpp)
void decorateOscillatorNodeProto(ObjectBuilder& b);
void decoratePeriodicWaveProto(ObjectBuilder& b);
void decorateBiquadFilterNodeProto(ObjectBuilder& b);
void decorateAnalyserNodeProto(ObjectBuilder& b);
void decorateAudioBufferSourceNodeProto(ObjectBuilder& b);
Value makeGainNodeValue();
Value makeOscillatorNodeValue();
Value makePeriodicWaveValue(const float* real, const float* imag, int count, bool disableNorm);
Value makeBiquadFilterNodeValue();
Value makeAnalyserNodeValue();
Value makeAudioBufferSourceNodeValue();

// Spatial creators & decorators (host_audio_spatial.cpp)
void decoratePannerNodeProto(ObjectBuilder& b);
void decorateStereoPannerNodeProto(ObjectBuilder& b);
Value makePannerNodeValue();
Value makeStereoPannerNodeValue();

// DSP creators & decorators (host_audio_dsp.cpp)
void decorateDelayNodeProto(ObjectBuilder& b);
void decorateDynamicsCompressorNodeProto(ObjectBuilder& b);
void decorateWaveShaperNodeProto(ObjectBuilder& b);
void decorateConvolverNodeProto(ObjectBuilder& b);
void decorateChannelSplitterNodeProto(ObjectBuilder& b);
void decorateChannelMergerNodeProto(ObjectBuilder& b);
Value makeDelayNodeValue(double maxDelayTime = 1.0);
Value makeDynamicsCompressorNodeValue();
Value makeWaveShaperNodeValue();
Value makeConvolverNodeValue();
Value makeChannelSplitterNodeValue(int numberOfOutputs = 6);
Value makeChannelMergerNodeValue(int numberOfInputs = 6);

// Helpers
broaudio::BiquadFilter::Type parseFilterType(const std::string& str);
const char* filterTypeToString(broaudio::BiquadFilter::Type type);
broaudio::Waveform parseWaveform(const std::string& str);
const char* waveformToString(broaudio::Waveform wf);

}  // namespace bro::bronze_host
