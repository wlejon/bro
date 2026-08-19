// Web Audio & Sound Engine Integration — DSP Audio Nodes
//
// DelayNode, DynamicsCompressorNode, WaveShaperNode, ConvolverNode,
// ChannelSplitterNode, ChannelMergerNode.

#include "bronze_host/host_audio_internal.h"

namespace bro::bronze_host {

// ---------------------------------------------------------------------------
// Destructors (Finalizers)
// ---------------------------------------------------------------------------

void hostDelayDtor(void* p) {
    delete static_cast<HostDelayNode*>(p);
}

void hostDynamicsCompressorDtor(void* p) {
    delete static_cast<HostDynamicsCompressorNode*>(p);
}

void hostWaveShaperDtor(void* p) {
    delete static_cast<HostWaveShaperNode*>(p);
}

void hostConvolverDtor(void* p) {
    delete static_cast<HostConvolverNode*>(p);
}

void hostChannelSplitterDtor(void* p) {
    delete static_cast<HostChannelSplitterNode*>(p);
}

void hostChannelMergerDtor(void* p) {
    delete static_cast<HostChannelMergerNode*>(p);
}

// ---------------------------------------------------------------------------
// DelayNode
// ---------------------------------------------------------------------------

void decorateDelayNodeProto(ObjectBuilder&) {
    // delayTime AudioParam provides all methods/accessors
}

Value makeDelayNodeValue(double maxDelayTime) {
    if (maxDelayTime <= 0.0) maxDelayTime = 1.0;
    if (maxDelayTime > 180.0) maxDelayTime = 180.0;

    auto* delay = new HostDelayNode();
    delay->base.nodeType = AudioNodeType::Delay;
    delay->maxDelayTime = maxDelayTime;

    ObjectBuilder b(g_delayNodeClass.make(delay, hostDelayDtor));

    b.set("delayTime", makeAudioParamValue(AudioParamTarget::DelayTime, -1, 0.0f, 0.0f, static_cast<float>(maxDelayTime), 0.0f));

    return b.get();
}

// ---------------------------------------------------------------------------
// DynamicsCompressorNode
// ---------------------------------------------------------------------------

void decorateDynamicsCompressorNodeProto(ObjectBuilder& b) {
    b.accessor("reduction", [](Value self_, std::span<const Value>) {
        HostDynamicsCompressorNode* comp = compressorOf(self_);
        if (!comp) return ev::fromDouble(0.0);
        return ev::fromDouble(comp->reduction);
    }, nullptr);
}

Value makeDynamicsCompressorNodeValue() {
    auto* comp = new HostDynamicsCompressorNode();
    comp->base.nodeType = AudioNodeType::DynamicsCompressor;

    ObjectBuilder b(g_dynamicsCompressorNodeClass.make(comp, hostDynamicsCompressorDtor));

    b.set("threshold", makeAudioParamValue(AudioParamTarget::CompressorThreshold, -1, -24.0f, -100.0f, 0.0f, -24.0f));
    b.set("knee", makeAudioParamValue(AudioParamTarget::CompressorKnee, -1, 30.0f, 0.0f, 40.0f, 30.0f));
    b.set("ratio", makeAudioParamValue(AudioParamTarget::CompressorRatio, -1, 12.0f, 1.0f, 20.0f, 12.0f));
    b.set("attack", makeAudioParamValue(AudioParamTarget::CompressorAttack, -1, 0.003f, 0.0f, 1.0f, 0.003f));
    b.set("release", makeAudioParamValue(AudioParamTarget::CompressorRelease, -1, 0.25f, 0.0f, 1.0f, 0.25f));

    return b.get();
}

// ---------------------------------------------------------------------------
// WaveShaperNode
// ---------------------------------------------------------------------------

void decorateWaveShaperNodeProto(ObjectBuilder& b) {
    b.accessor("curve",
               [](Value thisValue, std::span<const Value>) {
                   Value curveVal = ev::getProperty(thisValue, "_curve");
                   return ev::isObject(curveVal) ? curveVal : ev::null();
               },
               [](Value thisValue, std::span<const Value> a) {
                   HostWaveShaperNode* ws = waveShaperOf(thisValue);
                   if (!ws) return ev::undefined();
                   Value v = argAt(a, 0);
                   ev::Persistent self(thisValue);
                   if (ev::isTypedArray(v)) {
                       // Sized off byteLength, never elementCount: the two are
                       // equal only for a Float32Array, and `elementCount *
                       // sizeof(float)` reads four bytes per element out of an
                       // Int8Array that holds one — twelve bytes past the end of
                       // the buffer. floatData() in gl_internal.h is the same
                       // rule, and every other typed-array reader here follows
                       // it. The spec says Float32Array; a program is free to
                       // pass something else and must not be able to walk the
                       // heap by doing so.
                       ev::TypedArrayInfo info = ev::typedArrayInfo(v);
                       if (info && info.data && info.byteLength >= sizeof(float)) {
                           const size_t count = info.byteLength / sizeof(float);
                           ws->curve.resize(count);
                           std::memcpy(ws->curve.data(), info.data, count * sizeof(float));
                           ev::Persistent curveVal(v);
                           ev::setProperty(self.get(), "_curve", curveVal.get());
                       }
                   } else {
                       ws->curve.clear();
                       ev::setProperty(self.get(), "_curve", ev::null());
                   }
                   return ev::undefined();
               });

    b.accessor("oversample",
               [](Value self_, std::span<const Value>) {
                   HostWaveShaperNode* ws = waveShaperOf(self_);
                   if (!ws) return ev::undefined();
                   return ev::fromUtf8(ws->oversample);
               },
               [](Value self_, std::span<const Value> a) {
                   HostWaveShaperNode* ws = waveShaperOf(self_);
                   if (!ws || a.empty() || ev::isObject(a[0]) || ev::isUndefined(a[0])) return ev::undefined();
                   std::string s = ev::toUtf8(a[0]);
                   if (s == "none" || s == "2x" || s == "4x") {
                       ws->oversample = s;
                   }
                   return ev::undefined();
               });
}

Value makeWaveShaperNodeValue() {
    auto* ws = new HostWaveShaperNode();
    ws->base.nodeType = AudioNodeType::WaveShaper;

    ObjectBuilder b(g_waveShaperNodeClass.make(ws, hostWaveShaperDtor));
    b.set("_curve", ev::null());
    return b.get();
}

// ---------------------------------------------------------------------------
// ConvolverNode
// ---------------------------------------------------------------------------

void decorateConvolverNodeProto(ObjectBuilder& b) {
    b.accessor("buffer",
               [](Value thisValue, std::span<const Value>) {
                   Value buf = ev::getProperty(thisValue, "_buffer");
                   return ev::isObject(buf) ? buf : ev::null();
               },
               [](Value thisValue, std::span<const Value> a) {
                   HostConvolverNode* conv = convolverOf(thisValue);
                   if (!conv) return ev::undefined();
                   Value v = argAt(a, 0);
                   ev::Persistent self(thisValue);
                   ev::Persistent bufVal(v);
                   if (auto* b = hostAudioBufferOf(v)) {
                       conv->buffer = b;
                       ev::setProperty(self.get(), "_buffer", bufVal.get());
                   } else {
                       conv->buffer = nullptr;
                       ev::setProperty(self.get(), "_buffer", ev::null());
                   }
                   return ev::undefined();
               });

    b.accessor("normalize",
               [](Value self_, std::span<const Value>) {
                   HostConvolverNode* conv = convolverOf(self_);
                   if (!conv) return ev::undefined();
                   return ev::fromBool(conv->normalize);
               },
               [](Value self_, std::span<const Value> a) {
                   HostConvolverNode* conv = convolverOf(self_);
                   if (!conv) return ev::undefined();
                   conv->normalize = boolAt(a, 0);
                   return ev::undefined();
               });
}

Value makeConvolverNodeValue() {
    auto* conv = new HostConvolverNode();
    conv->base.nodeType = AudioNodeType::Convolver;

    ObjectBuilder b(g_convolverNodeClass.make(conv, hostConvolverDtor));
    b.set("_buffer", ev::null());
    return b.get();
}

// ---------------------------------------------------------------------------
// ChannelSplitterNode
// ---------------------------------------------------------------------------

void decorateChannelSplitterNodeProto(ObjectBuilder&) {
    // numberOfOutputs is handled on AudioNode.numberOfOutputs
}

Value makeChannelSplitterNodeValue(int numberOfOutputs) {
    if (numberOfOutputs <= 0) numberOfOutputs = 6;
    if (numberOfOutputs > 32) numberOfOutputs = 32;

    auto* splitter = new HostChannelSplitterNode();
    splitter->base.nodeType = AudioNodeType::ChannelSplitter;
    splitter->numberOfOutputs = numberOfOutputs;

    return g_channelSplitterNodeClass.make(splitter, hostChannelSplitterDtor);
}

// ---------------------------------------------------------------------------
// ChannelMergerNode
// ---------------------------------------------------------------------------

void decorateChannelMergerNodeProto(ObjectBuilder&) {
    // numberOfInputs is handled on AudioNode.numberOfInputs
}

Value makeChannelMergerNodeValue(int numberOfInputs) {
    if (numberOfInputs <= 0) numberOfInputs = 6;
    if (numberOfInputs > 32) numberOfInputs = 32;

    auto* merger = new HostChannelMergerNode();
    merger->base.nodeType = AudioNodeType::ChannelMerger;
    merger->numberOfInputs = numberOfInputs;

    return g_channelMergerNodeClass.make(merger, hostChannelMergerDtor);
}

}  // namespace bro::bronze_host
