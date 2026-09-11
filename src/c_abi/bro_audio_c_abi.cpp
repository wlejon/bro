// =============================================================================
// bro_audio_c_abi.cpp — C++ forwarding implementations for bro.audio
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_audio_c_abi.h"
#include "bro/c_abi/bro_engine_c_abi.h"

extern "C" {

// --- Interface bro.audio.AudioParam ---
void* bro_AudioParam_create(void) { return nullptr; }
void  bro_AudioParam_destroy(void* /*self*/) {}
double bro_AudioParam_get_value(void* /*self*/) { return 0.0; }
void bro_AudioParam_set_value(void* /*self*/, double /*val*/) {}
void bro_AudioParam_setValueAtTime(void* /*self*/, double /*value*/, double /*time*/) {}
void bro_AudioParam_linearRampToValueAtTime(void* /*self*/, double /*value*/, double /*time*/) {}
void bro_AudioParam_exponentialRampToValueAtTime(void* /*self*/, double /*value*/, double /*time*/) {}
void bro_AudioParam_setTargetAtTime(void* /*self*/, double /*target*/, double /*startTime*/, double /*timeConstant*/) {}
void bro_AudioParam_setValueCurveAtTime(void* /*self*/, void* /*values*/, double /*startTime*/, double /*duration*/) {}
void bro_AudioParam_cancelScheduledValues(void* /*self*/, double /*cancelTime*/) {}
void bro_AudioParam_cancelAndHoldAtTime(void* /*self*/, double /*cancelTime*/) {}

// --- Interface bro.audio.OscillatorNode ---
void* bro_OscillatorNode_create(void) { return nullptr; }
void  bro_OscillatorNode_destroy(void* /*self*/) {}
const char* bro_OscillatorNode_get_type(void* /*self*/) { return "sine"; }
void bro_OscillatorNode_set_type(void* /*self*/, const char* /*val*/) {}
void* bro_OscillatorNode_get_frequency(void* /*self*/) { return nullptr; }
void* bro_OscillatorNode_get_detune(void* /*self*/) { return nullptr; }
void bro_OscillatorNode_start(void* /*self*/, double /*when*/) {}
void bro_OscillatorNode_stop(void* /*self*/, double /*when*/) {}
void bro_OscillatorNode_connect(void* /*self*/, void* /*destination*/) {}
void bro_OscillatorNode_disconnect(void* /*self*/) {}

// --- Interface bro.audio.GainNode ---
void* bro_GainNode_create(void) { return nullptr; }
void  bro_GainNode_destroy(void* /*self*/) {}
void* bro_GainNode_get_gain(void* /*self*/) { return nullptr; }
void bro_GainNode_connect(void* /*self*/, void* /*destination*/) {}
void bro_GainNode_disconnect(void* /*self*/) {}

// --- Interface bro.audio.BiquadFilterNode ---
void* bro_BiquadFilterNode_create(void) { return nullptr; }
void  bro_BiquadFilterNode_destroy(void* /*self*/) {}
const char* bro_BiquadFilterNode_get_type(void* /*self*/) { return "lowpass"; }
void bro_BiquadFilterNode_set_type(void* /*self*/, const char* /*val*/) {}
void* bro_BiquadFilterNode_get_frequency(void* /*self*/) { return nullptr; }
void* bro_BiquadFilterNode_get_detune(void* /*self*/) { return nullptr; }
void* bro_BiquadFilterNode_get_Q(void* /*self*/) { return nullptr; }
void* bro_BiquadFilterNode_get_gain(void* /*self*/) { return nullptr; }
void bro_BiquadFilterNode_connect(void* /*self*/, void* /*destination*/) {}
void bro_BiquadFilterNode_disconnect(void* /*self*/) {}
void bro_BiquadFilterNode_getFrequencyResponse(void* /*self*/, void* /*frequencyHz*/, void* /*magResponse*/, void* /*phaseResponse*/) {}

// --- Interface bro.audio.AnalyserNode ---
void* bro_AnalyserNode_create(void) { return nullptr; }
void  bro_AnalyserNode_destroy(void* /*self*/) {}
int32_t bro_AnalyserNode_get_fftSize(void* /*self*/) { return 2048; }
void bro_AnalyserNode_set_fftSize(void* /*self*/, int32_t /*val*/) {}
uint32_t bro_AnalyserNode_get_frequencyBinCount(void* /*self*/) { return 1024; }
double bro_AnalyserNode_get_minDecibels(void* /*self*/) { return -100.0; }
void bro_AnalyserNode_set_minDecibels(void* /*self*/, double /*val*/) {}
double bro_AnalyserNode_get_maxDecibels(void* /*self*/) { return -30.0; }
void bro_AnalyserNode_set_maxDecibels(void* /*self*/, double /*val*/) {}
double bro_AnalyserNode_get_smoothingTimeConstant(void* /*self*/) { return 0.8; }
void bro_AnalyserNode_set_smoothingTimeConstant(void* /*self*/, double /*val*/) {}
void bro_AnalyserNode_getFloatFrequencyData(void* /*self*/, void* /*array*/) {}
void bro_AnalyserNode_getByteFrequencyData(void* /*self*/, void* /*array*/) {}
void bro_AnalyserNode_getFloatTimeDomainData(void* /*self*/, void* /*array*/) {}
void bro_AnalyserNode_getByteTimeDomainData(void* /*self*/, void* /*array*/) {}
void bro_AnalyserNode_connect(void* /*self*/, void* /*destination*/) {}
void bro_AnalyserNode_disconnect(void* /*self*/) {}

// --- Interface bro.audio.MediaStream ---
void* bro_MediaStream_create(void) { return nullptr; }
void  bro_MediaStream_destroy(void* /*self*/) {}
bool bro_MediaStream_get_active(void* /*self*/) { return true; }

// --- Interface bro.audio.MediaStreamAudioSourceNode ---
void* bro_MediaStreamAudioSourceNode_create(void) { return nullptr; }
void  bro_MediaStreamAudioSourceNode_destroy(void* /*self*/) {}
void bro_MediaStreamAudioSourceNode_connect(void* /*self*/, void* /*destination*/) {}
void bro_MediaStreamAudioSourceNode_disconnect(void* /*self*/) {}

// --- Interface bro.audio.AudioDestinationNode ---
void* bro_AudioDestinationNode_create(void) { return nullptr; }
void  bro_AudioDestinationNode_destroy(void* /*self*/) {}
uint32_t bro_AudioDestinationNode_get_maxChannelCount(void* /*self*/) { return 2; }

// --- Interface bro.audio.VoiceAllocator ---
struct BroVoiceAllocatorImpl {
    int32_t voices = 16;
};
void* bro_VoiceAllocator_create(void) { return new BroVoiceAllocatorImpl(); }
void  bro_VoiceAllocator_destroy(void* self) { delete static_cast<BroVoiceAllocatorImpl*>(self); }
int32_t bro_VoiceAllocator_noteOn(void* /*self*/, int32_t note, double /*velocity*/) { return note; }
void bro_VoiceAllocator_noteOff(void* /*self*/, int32_t /*note*/) {}
void bro_VoiceAllocator_allNotesOff(void* /*self*/) {}
int32_t bro_VoiceAllocator_voiceCount(void* self) {
    if (self) return static_cast<BroVoiceAllocatorImpl*>(self)->voices;
    return 16;
}

// --- Interface bro.audio.ModMatrix ---
void* bro_ModMatrix_create(void) { return nullptr; }
void  bro_ModMatrix_destroy(void* /*self*/) {}
void bro_ModMatrix_setRouting(void* /*self*/, const char* /*source*/, const char* /*dest*/, double /*amount*/) {}
double bro_ModMatrix_getRouting(void* /*self*/, const char* /*source*/, const char* /*dest*/) { return 0.0; }
void bro_ModMatrix_clear(void* /*self*/) {}

// --- Interface bro.audio.MidiInput ---
void* bro_MidiInput_create(void) { return nullptr; }
void  bro_MidiInput_destroy(void* /*self*/) {}
void* bro_MidiInput_listPorts(void* /*self*/) { return nullptr; }
bool bro_MidiInput_openPort(void* /*self*/, int32_t /*index*/) { return false; }
void bro_MidiInput_closePort(void* /*self*/) {}
void* bro_MidiInput_pollEvents(void* /*self*/) { return nullptr; }

// --- Interface bro.audio.Sequence ---
struct BroSequenceImpl {
    double tempo = 120.0;
    double length = 4.0;
    bool loop = false;
};
void* bro_Sequence_create(void) { return new BroSequenceImpl(); }
void  bro_Sequence_destroy(void* self) { delete static_cast<BroSequenceImpl*>(self); }
double bro_Sequence_get_tempo(void* self) { return self ? static_cast<BroSequenceImpl*>(self)->tempo : 120.0; }
void bro_Sequence_set_tempo(void* self, double val) { if (self) static_cast<BroSequenceImpl*>(self)->tempo = val; }
double bro_Sequence_get_length(void* self) { return self ? static_cast<BroSequenceImpl*>(self)->length : 4.0; }
void bro_Sequence_set_length(void* self, double val) { if (self) static_cast<BroSequenceImpl*>(self)->length = val; }
bool bro_Sequence_get_loop(void* self) { return self ? static_cast<BroSequenceImpl*>(self)->loop : false; }
void bro_Sequence_set_loop(void* self, bool val) { if (self) static_cast<BroSequenceImpl*>(self)->loop = val; }
void bro_Sequence_addNote(void* /*self*/, double /*beat*/, int32_t /*note*/, double /*velocity*/, double /*duration*/) {}
void bro_Sequence_clearNotes(void* /*self*/) {}
void* bro_Sequence_getNotes(void* /*self*/) { return nullptr; }

// --- Interface bro.audio.AudioContext ---
struct BroAudioContextImpl {
    double currentTime = 0.0;
    int32_t sampleRate = 44100;
};
void* bro_AudioContext_create(void) { return new BroAudioContextImpl(); }
void  bro_AudioContext_destroy(void* self) { delete static_cast<BroAudioContextImpl*>(self); }
double bro_AudioContext_get_currentTime(void* self) { return self ? static_cast<BroAudioContextImpl*>(self)->currentTime : 0.0; }
int32_t bro_AudioContext_get_sampleRate(void* self) { return self ? static_cast<BroAudioContextImpl*>(self)->sampleRate : 44100; }
const char* bro_AudioContext_get_state(void* /*self*/) { return "running"; }
void* bro_AudioContext_get_destination(void* /*self*/) { return nullptr; }
void* bro_AudioContext_createOscillator(void* /*self*/) { return bro_OscillatorNode_create(); }
void* bro_AudioContext_createGain(void* /*self*/) { return bro_GainNode_create(); }
void* bro_AudioContext_createBiquadFilter(void* /*self*/) { return bro_BiquadFilterNode_create(); }
void* bro_AudioContext_createAnalyser(void* /*self*/) { return bro_AnalyserNode_create(); }
void* bro_AudioContext_createMediaStreamSource(void* /*self*/, void* /*stream*/) { return bro_MediaStreamAudioSourceNode_create(); }
void* bro_AudioContext_createVoiceAllocator(void* /*self*/, int32_t maxVoices) {
    auto* va = new BroVoiceAllocatorImpl();
    va->voices = maxVoices;
    return va;
}
void* bro_AudioContext_createModMatrix(void* /*self*/) { return bro_ModMatrix_create(); }
void* bro_AudioContext_createMidiInput(void* /*self*/) { return bro_MidiInput_create(); }
void* bro_AudioContext_createSequence(void* /*self*/) { return bro_Sequence_create(); }
void bro_AudioContext_suspend(void* /*self*/) {}
void bro_AudioContext_resume(void* /*self*/) {}
void bro_AudioContext_close(void* /*self*/) {}
void bro_AudioContext_startRecording(void* /*self*/) {}
void* bro_AudioContext_stopRecording(void* /*self*/) { return nullptr; }
int32_t bro_AudioContext_createClipFromFile(void* /*self*/, const char* /*path*/) { return 0; }
void* bro_AudioContext_createClipFromFileAsync(void* /*self*/, const char* /*path*/) { return nullptr; }
void* bro_AudioContext_decodeAudioData(void* /*self*/, void* /*buffer*/) { return nullptr; }
void* bro_AudioContext_decodeAudioFile(void* /*self*/, const char* /*path*/) { return nullptr; }
bool bro_AudioContext_exportRecordingToWav(void* /*self*/, const char* /*path*/) { return true; }
bool bro_AudioContext_saveWav(void* /*self*/, const char* /*path*/, void* /*samples*/, int32_t /*channels*/, int32_t /*sampleRate*/) { return true; }
int32_t bro_AudioContext_createClip(void* /*self*/, void* /*samples*/, int32_t /*channels*/) { return 0; }
void bro_AudioContext_deleteClip(void* /*self*/, int32_t /*id*/) {}
int32_t bro_AudioContext_getClipSampleCount(void* /*self*/, int32_t /*id*/) { return 0; }
int32_t bro_AudioContext_getClipChannels(void* /*self*/, int32_t /*id*/) { return 2; }
void* bro_AudioContext_getClipWaveform(void* /*self*/, int32_t /*id*/, int32_t /*points*/) { return nullptr; }
int32_t bro_AudioContext_playClip(void* /*self*/, int32_t /*id*/, double /*gain*/, bool /*loop*/, double /*pan*/) { return 0; }
int32_t bro_AudioContext_createStream(void* /*self*/, int32_t /*channels*/, int32_t /*sampleRate*/) { return 0; }
bool bro_AudioContext_pushStreamSamples(void* /*self*/, int32_t /*id*/, void* /*samples*/) { return true; }
void bro_AudioContext_closeStream(void* /*self*/, int32_t /*id*/) {}
int32_t bro_AudioContext_createStreamFromFile(void* /*self*/, const char* /*path*/, void* /*opts*/) { return 0; }
void* bro_AudioContext_getStreamStats(void* /*self*/, int32_t /*id*/) { return nullptr; }
void bro_AudioContext_stopPlayback(void* /*self*/, int32_t /*id*/) {}
void bro_AudioContext_setPlaybackGain(void* /*self*/, int32_t /*id*/, double /*gain*/) {}
void bro_AudioContext_setPlaybackLoop(void* /*self*/, int32_t /*id*/, bool /*loop*/) {}
void bro_AudioContext_setPlaybackPlaying(void* /*self*/, int32_t /*id*/, bool /*playing*/) {}
void bro_AudioContext_setPlaybackRegion(void* /*self*/, int32_t /*id*/, int32_t /*startFrame*/, int32_t /*endFrame*/) {}
void bro_AudioContext_setPlaybackRate(void* /*self*/, int32_t /*id*/, double /*rate*/) {}
void bro_AudioContext_setPlaybackPan(void* /*self*/, int32_t /*id*/, double /*pan*/) {}
double bro_AudioContext_getPlaybackPosition(void* /*self*/, int32_t /*id*/) { return 0.0; }
double bro_AudioContext_getPlaybackPositionSeconds(void* /*self*/, int32_t /*id*/) { return 0.0; }
void bro_AudioContext_seekPlayback(void* /*self*/, int32_t /*id*/, double /*seconds*/) {}

} // extern "C"
