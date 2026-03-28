#include "js/audio_bindings.h"
#include "js/runtime.h"
#include "audio/audio_engine.h"

#include <string>
#include <cstring>

namespace bro::js {

// ---------------------------------------------------------------------------
// Class IDs
// ---------------------------------------------------------------------------

static JSClassID js_audioctx_class_id = 0;
static JSClassID js_oscnode_class_id = 0;
static JSClassID js_gainnode_class_id = 0;
static JSClassID js_destnode_class_id = 0;
static JSClassID js_audioparam_class_id = 0;

// ---------------------------------------------------------------------------
// AudioParam — wraps a float value that updates the engine
// ---------------------------------------------------------------------------

struct AudioParamData {
    audio::AudioEngine* engine;
    int voiceId;
    enum class Target { Frequency, Gain } target;
    float value;
};

static void js_audioparam_finalizer(JSRuntime*, JSValue val) {
    delete static_cast<AudioParamData*>(JS_GetOpaque(val, js_audioparam_class_id));
}

static JSClassDef js_audioparam_class = {
    "AudioParam", js_audioparam_finalizer, nullptr, nullptr, nullptr
};

static JSValue js_audioparam_get_value(JSContext* ctx, JSValueConst this_val) {
    auto* p = static_cast<AudioParamData*>(JS_GetOpaque(this_val, js_audioparam_class_id));
    return p ? JS_NewFloat64(ctx, p->value) : JS_UNDEFINED;
}

static JSValue js_audioparam_set_value(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* p = static_cast<AudioParamData*>(JS_GetOpaque(this_val, js_audioparam_class_id));
    if (!p) return JS_UNDEFINED;
    double v; JS_ToFloat64(ctx, &v, val);
    p->value = static_cast<float>(v);
    if (p->target == AudioParamData::Target::Frequency)
        p->engine->setFrequency(p->voiceId, p->value);
    else
        p->engine->setGain(p->voiceId, p->value);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_audioparam_proto_funcs[] = {
    JS_CGETSET_DEF("value", js_audioparam_get_value, js_audioparam_set_value),
};

static JSValue createAudioParam(JSContext* ctx, audio::AudioEngine* engine,
                                int voiceId, AudioParamData::Target target, float initial) {
    JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(js_audioparam_class_id));
    auto* data = new AudioParamData{engine, voiceId, target, initial};
    JS_SetOpaque(obj, data);
    return obj;
}

// ---------------------------------------------------------------------------
// AudioDestinationNode — just a marker for connect() target
// ---------------------------------------------------------------------------

static JSClassDef js_destnode_class = {
    "AudioDestinationNode", nullptr, nullptr, nullptr, nullptr
};

// ---------------------------------------------------------------------------
// OscillatorNode — wraps a voice in the AudioEngine
// ---------------------------------------------------------------------------

struct OscNodeData {
    audio::AudioEngine* engine;
    int voiceId;
    std::string type = "sine";
};

static void js_oscnode_finalizer(JSRuntime*, JSValue val) {
    auto* d = static_cast<OscNodeData*>(JS_GetOpaque(val, js_oscnode_class_id));
    if (d) {
        d->engine->removeVoice(d->voiceId);
        delete d;
    }
}

static JSClassDef js_oscnode_class = {
    "OscillatorNode", js_oscnode_finalizer, nullptr, nullptr, nullptr
};

static JSValue js_osc_get_type(JSContext* ctx, JSValueConst this_val) {
    auto* d = static_cast<OscNodeData*>(JS_GetOpaque(this_val, js_oscnode_class_id));
    return d ? JS_NewString(ctx, d->type.c_str()) : JS_UNDEFINED;
}

static JSValue js_osc_set_type(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* d = static_cast<OscNodeData*>(JS_GetOpaque(this_val, js_oscnode_class_id));
    if (!d) return JS_UNDEFINED;
    const char* s = JS_ToCString(ctx, val);
    if (!s) return JS_UNDEFINED;
    d->type = s;
    audio::Waveform wf = audio::Waveform::Sine;
    if (d->type == "square") wf = audio::Waveform::Square;
    else if (d->type == "sawtooth") wf = audio::Waveform::Sawtooth;
    else if (d->type == "triangle") wf = audio::Waveform::Triangle;
    d->engine->setWaveform(d->voiceId, wf);
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

static JSValue js_osc_connect(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    auto* d = static_cast<OscNodeData*>(JS_GetOpaque(this_val, js_oscnode_class_id));
    if (!d) return JS_UNDEFINED;

    // If connecting to a GainNode, read its gain param and apply to voice
    // For now, connect() just returns the target for chaining
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_osc_start(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<OscNodeData*>(JS_GetOpaque(this_val, js_oscnode_class_id));
    if (!d) return JS_UNDEFINED;
    double when = d->engine->currentTime();
    if (argc >= 1) JS_ToFloat64(ctx, &when, argv[0]);
    d->engine->startVoice(d->voiceId, when);
    return JS_UNDEFINED;
}

static JSValue js_osc_stop(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* d = static_cast<OscNodeData*>(JS_GetOpaque(this_val, js_oscnode_class_id));
    if (!d) return JS_UNDEFINED;
    double when = d->engine->currentTime();
    if (argc >= 1) JS_ToFloat64(ctx, &when, argv[0]);
    d->engine->stopVoice(d->voiceId, when);
    return JS_UNDEFINED;
}

static JSValue js_osc_disconnect(JSContext*, JSValueConst, int, JSValueConst*) {
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_oscnode_proto_funcs[] = {
    JS_CGETSET_DEF("type", js_osc_get_type, js_osc_set_type),
    JS_CFUNC_DEF("connect", 1, js_osc_connect),
    JS_CFUNC_DEF("disconnect", 0, js_osc_disconnect),
    JS_CFUNC_DEF("start", 1, js_osc_start),
    JS_CFUNC_DEF("stop", 1, js_osc_stop),
};

// ---------------------------------------------------------------------------
// GainNode — wraps a gain value applied to connected oscillators
// ---------------------------------------------------------------------------

struct GainNodeData {
    audio::AudioEngine* engine;
    // List of voice IDs this gain node controls
    // (set when oscillators connect through this node)
};

static void js_gainnode_finalizer(JSRuntime*, JSValue val) {
    delete static_cast<GainNodeData*>(JS_GetOpaque(val, js_gainnode_class_id));
}

static JSClassDef js_gainnode_class = {
    "GainNode", js_gainnode_finalizer, nullptr, nullptr, nullptr
};

static JSValue js_gain_connect(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_gain_disconnect(JSContext*, JSValueConst, int, JSValueConst*) {
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_gainnode_proto_funcs[] = {
    JS_CFUNC_DEF("connect", 1, js_gain_connect),
    JS_CFUNC_DEF("disconnect", 0, js_gain_disconnect),
};

// ---------------------------------------------------------------------------
// AudioContext — main entry point
// ---------------------------------------------------------------------------

struct AudioCtxData {
    audio::AudioEngine* engine;
};

static void js_audioctx_finalizer(JSRuntime*, JSValue val) {
    delete static_cast<AudioCtxData*>(JS_GetOpaque(val, js_audioctx_class_id));
}

static JSClassDef js_audioctx_class = {
    "AudioContext", js_audioctx_finalizer, nullptr, nullptr, nullptr
};

static JSValue js_audioctx_get_currentTime(JSContext* ctx, JSValueConst this_val) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    return d ? JS_NewFloat64(ctx, d->engine->currentTime()) : JS_UNDEFINED;
}

static JSValue js_audioctx_get_sampleRate(JSContext* ctx, JSValueConst this_val) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    return d ? JS_NewInt32(ctx, d->engine->sampleRate()) : JS_UNDEFINED;
}

static JSValue js_audioctx_createOscillator(JSContext* ctx, JSValueConst this_val,
                                             int, JSValueConst*) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d) return JS_UNDEFINED;

    int voiceId = d->engine->createVoice();

    JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(js_oscnode_class_id));
    auto* oscData = new OscNodeData{d->engine, voiceId, "sine"};
    JS_SetOpaque(obj, oscData);

    // Create the frequency AudioParam
    JSValue freqParam = createAudioParam(ctx, d->engine, voiceId,
                                         AudioParamData::Target::Frequency, 440.0f);
    JS_SetPropertyStr(ctx, obj, "frequency", freqParam);

    return obj;
}

static JSValue js_audioctx_createGain(JSContext* ctx, JSValueConst this_val,
                                       int, JSValueConst*) {
    auto* d = static_cast<AudioCtxData*>(JS_GetOpaque(this_val, js_audioctx_class_id));
    if (!d) return JS_UNDEFINED;

    JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(js_gainnode_class_id));
    auto* gainData = new GainNodeData{d->engine};
    JS_SetOpaque(obj, gainData);

    // Create a "virtual" voice for the gain param — we use voice ID 0
    // which means it's not directly an oscillator. Instead, when an oscillator
    // connects to this gain node, we update the oscillator's gain.
    // For now, we store the gain value on the JS object and read it when needed.

    // The gain AudioParam just stores a value; the actual gain application
    // happens when we intercept connect() from oscillator to gain node.
    // We'll use a simpler model: the JS Tetris code will call
    // osc.connect(gain); gain.connect(dest); and read gain.gain.value
    // We need the connect chain to propagate gain to the oscillator.

    // Simple approach: create a "dummy" AudioParam that stores the value.
    // The Tetris code sets gain.gain.value before calling osc.start().
    // We hook into osc.connect(gain) to copy the gain value to the voice.

    JSValue gainParam = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, gainParam, "value", JS_NewFloat64(ctx, 1.0));
    JS_SetPropertyStr(ctx, obj, "gain", gainParam);

    return obj;
}

static const JSCFunctionListEntry js_audioctx_proto_funcs[] = {
    JS_CGETSET_DEF("currentTime", js_audioctx_get_currentTime, nullptr),
    JS_CGETSET_DEF("sampleRate", js_audioctx_get_sampleRate, nullptr),
    JS_CFUNC_DEF("createOscillator", 0, js_audioctx_createOscillator),
    JS_CFUNC_DEF("createGain", 0, js_audioctx_createGain),
};

// ---------------------------------------------------------------------------
// Constructor for `new AudioContext()`
// ---------------------------------------------------------------------------

static audio::AudioEngine* s_audioEngine = nullptr;

static JSValue js_audioctx_constructor(JSContext* ctx, JSValueConst new_target,
                                        int, JSValueConst*) {
    if (!s_audioEngine) return JS_ThrowInternalError(ctx, "Audio not initialized");

    JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(js_audioctx_class_id));
    if (JS_IsException(obj)) return obj;

    auto* data = new AudioCtxData{s_audioEngine};
    JS_SetOpaque(obj, data);

    // Create destination node
    JSValue dest = JS_NewObjectClass(ctx, static_cast<int>(js_destnode_class_id));
    JS_SetPropertyStr(ctx, obj, "destination", dest);

    return obj;
}

// ---------------------------------------------------------------------------
// Revised OscillatorNode.connect() — reads gain from GainNode
// ---------------------------------------------------------------------------

// We override the simple connect with one that actually applies gain
static JSValue js_osc_connect_v2(JSContext* ctx, JSValueConst this_val,
                                  int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    auto* d = static_cast<OscNodeData*>(JS_GetOpaque(this_val, js_oscnode_class_id));
    if (!d) return JS_DupValue(ctx, argv[0]);

    // Check if target is a GainNode — read its gain.value
    auto* gd = static_cast<GainNodeData*>(JS_GetOpaque(argv[0], js_gainnode_class_id));
    if (gd) {
        // Store reference to gain node on the oscillator so we can read it at start time
        JS_SetPropertyStr(ctx, this_val, "__gainNode", JS_DupValue(ctx, argv[0]));
    }

    return JS_DupValue(ctx, argv[0]);
}

// Revised start that reads gain from connected GainNode
static JSValue js_osc_start_v2(JSContext* ctx, JSValueConst this_val,
                                int argc, JSValueConst* argv) {
    auto* d = static_cast<OscNodeData*>(JS_GetOpaque(this_val, js_oscnode_class_id));
    if (!d) return JS_UNDEFINED;

    // Read gain from connected GainNode if any
    JSValue gnVal = JS_GetPropertyStr(ctx, this_val, "__gainNode");
    if (!JS_IsUndefined(gnVal) && !JS_IsNull(gnVal)) {
        JSValue gainParam = JS_GetPropertyStr(ctx, gnVal, "gain");
        if (!JS_IsUndefined(gainParam)) {
            JSValue gainVal = JS_GetPropertyStr(ctx, gainParam, "value");
            double g = 1.0;
            JS_ToFloat64(ctx, &g, gainVal);
            d->engine->setGain(d->voiceId, static_cast<float>(g));
            JS_FreeValue(ctx, gainVal);
        }
        JS_FreeValue(ctx, gainParam);
    }
    JS_FreeValue(ctx, gnVal);

    double when = d->engine->currentTime();
    if (argc >= 1) JS_ToFloat64(ctx, &when, argv[0]);
    d->engine->startVoice(d->voiceId, when);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

void AudioBindings::install(JSContext* ctx, audio::AudioEngine* engine)
{
    s_audioEngine = engine;
    JSRuntime* rt = JS_GetRuntime(ctx);

    // Register class IDs
    JS_NewClassID(rt, &js_audioparam_class_id);
    JS_NewClass(rt, js_audioparam_class_id, &js_audioparam_class);
    JSValue apProto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, apProto, js_audioparam_proto_funcs,
                               sizeof(js_audioparam_proto_funcs)/sizeof(js_audioparam_proto_funcs[0]));
    JS_SetClassProto(ctx, js_audioparam_class_id, apProto);

    JS_NewClassID(rt, &js_destnode_class_id);
    JS_NewClass(rt, js_destnode_class_id, &js_destnode_class);

    JS_NewClassID(rt, &js_oscnode_class_id);
    JS_NewClass(rt, js_oscnode_class_id, &js_oscnode_class);
    {
        // Build prototype with v2 connect/start that handle gain propagation
        static const JSCFunctionListEntry osc_funcs[] = {
            JS_CGETSET_DEF("type", js_osc_get_type, js_osc_set_type),
            JS_CFUNC_DEF("connect", 1, js_osc_connect_v2),
            JS_CFUNC_DEF("disconnect", 0, js_osc_disconnect),
            JS_CFUNC_DEF("start", 1, js_osc_start_v2),
            JS_CFUNC_DEF("stop", 1, js_osc_stop),
        };
        JSValue oscProto = JS_NewObject(ctx);
        JS_SetPropertyFunctionList(ctx, oscProto, osc_funcs,
                                   sizeof(osc_funcs) / sizeof(osc_funcs[0]));
        JS_SetClassProto(ctx, js_oscnode_class_id, oscProto);
    }

    JS_NewClassID(rt, &js_gainnode_class_id);
    JS_NewClass(rt, js_gainnode_class_id, &js_gainnode_class);
    JSValue gainProto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, gainProto, js_gainnode_proto_funcs,
                               sizeof(js_gainnode_proto_funcs)/sizeof(js_gainnode_proto_funcs[0]));
    JS_SetClassProto(ctx, js_gainnode_class_id, gainProto);

    JS_NewClassID(rt, &js_audioctx_class_id);
    JS_NewClass(rt, js_audioctx_class_id, &js_audioctx_class);
    JSValue ctxProto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, ctxProto, js_audioctx_proto_funcs,
                               sizeof(js_audioctx_proto_funcs)/sizeof(js_audioctx_proto_funcs[0]));
    JS_SetClassProto(ctx, js_audioctx_class_id, ctxProto);

    // Register AudioContext constructor on global
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_NewCFunction2(ctx, js_audioctx_constructor, "AudioContext", 0,
                                     JS_CFUNC_constructor, 0);
    JS_SetPropertyStr(ctx, global, "AudioContext", ctor);
    JS_FreeValue(ctx, global);
}

void AudioBindings::cleanup(JSContext*)
{
    s_audioEngine = nullptr;
}

} // namespace bro::js
