#include "bronze_host/host_audio_internal.h"
#include "util/asset_path.h"
#include <map>

namespace bro::bronze_host {

HostClass g_voiceAllocatorClass;
HostClass g_modMatrixClass;
HostClass g_midiInputClass;
HostClass g_mediaStreamClass;
HostClass g_mediaStreamAudioSourceNodeClass;

void hostVoiceAllocatorDtor(void* p) {
    auto* h = static_cast<HostVoiceAllocator*>(p);
    h->voiceSetupCallback.set(ev::undefined());
    delete h;
}

void hostModMatrixDtor(void* p) {
    delete static_cast<HostModMatrix*>(p);
}

void hostMidiInputDtor(void* p) {
    auto* h = static_cast<HostMidiInput*>(p);
    h->pitchBendCb.set(ev::undefined());
    h->rawCb.set(ev::undefined());
    for (auto& cb : h->ccCallbacks) cb.set(ev::undefined());
    delete h;
}

void hostMediaStreamDtor(void* p) {
    delete static_cast<HostMediaStream*>(p);
}

void hostMediaStreamAudioSourceNodeDtor(void* p) {
    delete static_cast<HostMediaStreamAudioSourceNode*>(p);
}

HostVoiceAllocator* hostVoiceAllocatorOf(Value v) {
    auto* h = static_cast<HostVoiceAllocator*>(ev::handleData(v));
    return (h && h->tag == kHostVoiceAllocatorTag) ? h : nullptr;
}

HostModMatrix* hostModMatrixOf(Value v) {
    auto* h = static_cast<HostModMatrix*>(ev::handleData(v));
    return (h && h->tag == kHostModMatrixTag) ? h : nullptr;
}

HostMidiInput* hostMidiInputOf(Value v) {
    auto* h = static_cast<HostMidiInput*>(ev::handleData(v));
    return (h && h->tag == kHostMidiInputTag) ? h : nullptr;
}

HostMediaStream* hostMediaStreamOf(Value v) {
    auto* h = static_cast<HostMediaStream*>(ev::handleData(v));
    return (h && h->tag == kHostMediaStreamTag) ? h : nullptr;
}

HostMediaStreamAudioSourceNode* hostMediaStreamNodeOf(Value v) {
    auto* h = static_cast<HostMediaStreamAudioSourceNode*>(ev::handleData(v));
    return (h && h->base.tag == kHostAudioNodeTag) ? h : nullptr;
}

static std::map<int, std::shared_ptr<broaudio::WavetableBank>> s_wavetables;
static int s_nextWavetableId = 1;

std::shared_ptr<broaudio::WavetableBank> findWavetable(int id) {
    auto it = s_wavetables.find(id);
    return it != s_wavetables.end() ? it->second : nullptr;
}

int registerWavetable(std::shared_ptr<broaudio::WavetableBank> bank) {
    if (!bank) return -1;
    int id = s_nextWavetableId++;
    s_wavetables[id] = std::move(bank);
    return id;
}

void deleteWavetable(int id) {
    s_wavetables.erase(id);
}


broaudio::DistortionMode parseDistortionMode(const std::string& str) {
    if (str == "hardclip") return broaudio::DistortionMode::HardClip;
    if (str == "foldback") return broaudio::DistortionMode::Foldback;
    if (str == "bitcrush") return broaudio::DistortionMode::Bitcrush;
    return broaudio::DistortionMode::SoftClip;
}

const char* distortionModeToString(broaudio::DistortionMode mode) {
    switch (mode) {
        case broaudio::DistortionMode::HardClip: return "hardclip";
        case broaudio::DistortionMode::Foldback: return "foldback";
        case broaudio::DistortionMode::Bitcrush: return "bitcrush";
        default: return "softclip";
    }
}

broaudio::LfoShape parseLfoShape(const std::string& str) {
    if (str == "triangle") return broaudio::LfoShape::Triangle;
    if (str == "square") return broaudio::LfoShape::Square;
    if (str == "sawup") return broaudio::LfoShape::SawUp;
    if (str == "sawdown") return broaudio::LfoShape::SawDown;
    if (str == "sampleandhold") return broaudio::LfoShape::SampleAndHold;
    return broaudio::LfoShape::Sine;
}

const char* lfoShapeToString(broaudio::LfoShape shape) {
    switch (shape) {
        case broaudio::LfoShape::Triangle: return "triangle";
        case broaudio::LfoShape::Square: return "square";
        case broaudio::LfoShape::SawUp: return "sawup";
        case broaudio::LfoShape::SawDown: return "sawdown";
        case broaudio::LfoShape::SampleAndHold: return "sampleandhold";
        default: return "sine";
    }
}

broaudio::ModSource parseModSource(const std::string& str) {
    if (str == "lfo1") return broaudio::ModSource::Lfo1;
    if (str == "lfo2") return broaudio::ModSource::Lfo2;
    if (str == "lfo3") return broaudio::ModSource::Lfo3;
    if (str == "lfo4") return broaudio::ModSource::Lfo4;
    if (str == "envelope") return broaudio::ModSource::Envelope;
    if (str == "velocity") return broaudio::ModSource::Velocity;
    if (str == "keytracking") return broaudio::ModSource::KeyTracking;
    if (str == "modwheel") return broaudio::ModSource::ModWheel;
    if (str == "aftertouch") return broaudio::ModSource::Aftertouch;
    return broaudio::ModSource::Lfo1;
}

const char* modSourceToString(broaudio::ModSource src) {
    switch (src) {
        case broaudio::ModSource::Lfo1: return "lfo1";
        case broaudio::ModSource::Lfo2: return "lfo2";
        case broaudio::ModSource::Lfo3: return "lfo3";
        case broaudio::ModSource::Lfo4: return "lfo4";
        case broaudio::ModSource::Envelope: return "envelope";
        case broaudio::ModSource::Velocity: return "velocity";
        case broaudio::ModSource::KeyTracking: return "keytracking";
        case broaudio::ModSource::ModWheel: return "modwheel";
        case broaudio::ModSource::Aftertouch: return "aftertouch";
        default: return "lfo1";
    }
}

broaudio::ModDest parseModDest(const std::string& str) {
    if (str == "gain") return broaudio::ModDest::Gain;
    if (str == "pan") return broaudio::ModDest::Pan;
    if (str == "filterfreq") return broaudio::ModDest::FilterFreq;
    if (str == "filterq") return broaudio::ModDest::FilterQ;
    if (str == "pulsewidth") return broaudio::ModDest::PulseWidth;
    if (str == "delaysend") return broaudio::ModDest::DelaySend;
    return broaudio::ModDest::Pitch;
}

const char* modDestToString(broaudio::ModDest dst) {
    switch (dst) {
        case broaudio::ModDest::Gain: return "gain";
        case broaudio::ModDest::Pan: return "pan";
        case broaudio::ModDest::FilterFreq: return "filterfreq";
        case broaudio::ModDest::FilterQ: return "filterq";
        case broaudio::ModDest::PulseWidth: return "pulsewidth";
        case broaudio::ModDest::DelaySend: return "delaysend";
        default: return "pitch";
    }
}

broaudio::DistanceModel parseDistanceModel(const std::string& str) {
    if (str == "linear") return broaudio::DistanceModel::Linear;
    if (str == "exponential") return broaudio::DistanceModel::Exponential;
    return broaudio::DistanceModel::Inverse;
}

const char* distanceModelToString(broaudio::DistanceModel model) {
    switch (model) {
        case broaudio::DistanceModel::Linear: return "linear";
        case broaudio::DistanceModel::Exponential: return "exponential";
        default: return "inverse";
    }
}

broaudio::EffectSlot parseEffectSlot(const std::string& str, broaudio::EffectSlot def) {
    if (str == "filter") return broaudio::EffectSlot::Filter;
    if (str == "delay") return broaudio::EffectSlot::Delay;
    if (str == "compressor") return broaudio::EffectSlot::Compressor;
    if (str == "chorus") return broaudio::EffectSlot::Chorus;
    if (str == "reverb") return broaudio::EffectSlot::Reverb;
    if (str == "equalizer" || str == "eq") return broaudio::EffectSlot::Equalizer;
    if (str == "distortion") return broaudio::EffectSlot::Distortion;
    return def;
}

const char* effectSlotToString(broaudio::EffectSlot slot) {
    switch (slot) {
        case broaudio::EffectSlot::Filter: return "filter";
        case broaudio::EffectSlot::Delay: return "delay";
        case broaudio::EffectSlot::Compressor: return "compressor";
        case broaudio::EffectSlot::Chorus: return "chorus";
        case broaudio::EffectSlot::Reverb: return "reverb";
        case broaudio::EffectSlot::Equalizer: return "equalizer";
        case broaudio::EffectSlot::Distortion: return "distortion";
        default: return "filter";
    }
}

broaudio::StealPolicy parseStealPolicy(const std::string& str) {
    if (str == "quietest") return broaudio::StealPolicy::Quietest;
    if (str == "samenote") return broaudio::StealPolicy::SameNote;
    if (str == "none") return broaudio::StealPolicy::None;
    return broaudio::StealPolicy::Oldest;
}

static void decorateVoiceAllocatorProto(ObjectBuilder& b) {
    b.def("noteOn", 3, [](Value self, std::span<const Value> a) -> Value {
        auto* h = hostVoiceAllocatorOf(self);
        if (!h || !h->allocator || a.empty()) return ev::fromDouble(-1);
        int note = i32At(a, 0);
        float vel = a.size() >= 2 ? static_cast<float>(numAt(a, 1)) : 1.0f;
        double when = a.size() >= 3 ? numAt(a, 2) : 0.0;
        int voiceId = h->allocator->noteOn(note, vel, when);
        return ev::fromDouble(voiceId);
    });

    b.def("noteOff", 2, [](Value self, std::span<const Value> a) {
        auto* h = hostVoiceAllocatorOf(self);
        if (h && h->allocator && !a.empty()) {
            double when = a.size() >= 2 ? numAt(a, 1) : 0.0;
            h->allocator->noteOff(i32At(a, 0), when);
        }
        return ev::undefined();
    });

    b.def("allNotesOff", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostVoiceAllocatorOf(self);
        if (h && h->allocator) {
            double when = !a.empty() ? numAt(a, 0) : 0.0;
            h->allocator->allNotesOff(when);
        }
        return ev::undefined();
    });

    b.def("setStealPolicy", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostVoiceAllocatorOf(self);
        if (h && h->allocator && !a.empty()) {
            h->allocator->setStealPolicy(parseStealPolicy(ev::toUtf8(a[0])));
        }
        return ev::undefined();
    });

    b.def("setMaxVoices", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostVoiceAllocatorOf(self);
        if (h && h->allocator && !a.empty()) {
            h->allocator->setMaxVoices(i32At(a, 0));
        }
        return ev::undefined();
    });

    b.def("setVoiceSetup", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostVoiceAllocatorOf(self);
        if (!h || !h->allocator) return ev::undefined();
        if (!a.empty() && ev::isFunction(a[0])) {
            h->voiceSetupCallback = ev::Persistent(a[0]);
            h->allocator->setVoiceSetup([h](int voiceId, int note, float vel) {
                if (h && ev::isFunction(h->voiceSetupCallback.get())) {
                    Value args[3] = { ev::fromDouble(voiceId), ev::fromDouble(note), ev::fromDouble(vel) };
                    ev::call(h->voiceSetupCallback.get(), ev::undefined(), std::span<const Value>(args, 3));
                }
            });
        } else {
            h->voiceSetupCallback.set(ev::undefined());
            h->allocator->setVoiceSetup(nullptr);
        }
        return ev::undefined();
    });

    b.def("voiceForNote", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostVoiceAllocatorOf(self);
        return ev::fromDouble(h && h->allocator && !a.empty() ? h->allocator->voiceForNote(i32At(a, 0)) : -1);
    });

    b.accessor("activeVoiceCount", [](Value self, std::span<const Value>) {
        auto* h = hostVoiceAllocatorOf(self);
        return ev::fromDouble(h && h->allocator ? h->allocator->activeVoiceCount() : 0);
    }, nullptr);
}
static void decorateModMatrixProto(ObjectBuilder& b) {
    b.def("setLfoShape", 2, [](Value self, std::span<const Value> a) {
        auto* h = hostModMatrixOf(self);
        if (h && h->matrix && a.size() >= 2) {
            int idx = i32At(a, 0);
            if (idx >= 0 && idx < broaudio::ModState::MAX_LFOS) {
                h->matrix->setLfoShape(idx, parseLfoShape(ev::toUtf8(a[1])));
            }
        }
        return ev::undefined();
    });

    b.def("setLfoRate", 2, [](Value self, std::span<const Value> a) {
        auto* h = hostModMatrixOf(self);
        if (h && h->matrix && a.size() >= 2) {
            int idx = i32At(a, 0);
            if (idx >= 0 && idx < broaudio::ModState::MAX_LFOS) {
                h->matrix->setLfoRate(idx, static_cast<float>(numAt(a, 1)));
            }
        }
        return ev::undefined();
    });

    b.def("setLfoDepth", 2, [](Value self, std::span<const Value> a) {
        auto* h = hostModMatrixOf(self);
        if (h && h->matrix && a.size() >= 2) {
            int idx = i32At(a, 0);
            if (idx >= 0 && idx < broaudio::ModState::MAX_LFOS) {
                h->matrix->setLfoDepth(idx, static_cast<float>(numAt(a, 1)));
            }
        }
        return ev::undefined();
    });

    b.def("setLfoOffset", 2, [](Value self, std::span<const Value> a) {
        auto* h = hostModMatrixOf(self);
        if (h && h->matrix && a.size() >= 2) {
            int idx = i32At(a, 0);
            if (idx >= 0 && idx < broaudio::ModState::MAX_LFOS) {
                h->matrix->setLfoOffset(idx, static_cast<float>(numAt(a, 1)));
            }
        }
        return ev::undefined();
    });

    b.def("setLfoBipolar", 2, [](Value self, std::span<const Value> a) {
        auto* h = hostModMatrixOf(self);
        if (h && h->matrix && a.size() >= 2) {
            int idx = i32At(a, 0);
            if (idx >= 0 && idx < broaudio::ModState::MAX_LFOS) {
                h->matrix->setLfoBipolar(idx, boolAt(a, 1));
            }
        }
        return ev::undefined();
    });

    b.def("setLfoSync", 2, [](Value self, std::span<const Value> a) {
        auto* h = hostModMatrixOf(self);
        if (h && h->matrix && a.size() >= 2) {
            int idx = i32At(a, 0);
            if (idx >= 0 && idx < broaudio::ModState::MAX_LFOS) {
                h->matrix->setLfoSync(idx, boolAt(a, 1));
            }
        }
        return ev::undefined();
    });

    b.def("addRoute", 3, [](Value self, std::span<const Value> a) {
        auto* h = hostModMatrixOf(self);
        if (!h || !h->matrix || a.size() < 3) return ev::fromDouble(-1);
        auto src = parseModSource(ev::toUtf8(a[0]));
        auto dst = parseModDest(ev::toUtf8(a[1]));
        float amount = static_cast<float>(numAt(a, 2));
        int id = h->matrix->addRoute(src, dst, amount);
        return ev::fromDouble(id);
    });

    b.def("removeRoute", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostModMatrixOf(self);
        if (h && h->matrix && !a.empty()) h->matrix->removeRoute(i32At(a, 0));
        return ev::undefined();
    });

    b.def("setRouteAmount", 2, [](Value self, std::span<const Value> a) {
        auto* h = hostModMatrixOf(self);
        if (h && h->matrix && a.size() >= 2) {
            h->matrix->setRouteAmount(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        }
        return ev::undefined();
    });

    b.def("setRouteEnabled", 2, [](Value self, std::span<const Value> a) {
        auto* h = hostModMatrixOf(self);
        if (h && h->matrix && a.size() >= 2) {
            h->matrix->setRouteEnabled(i32At(a, 0), boolAt(a, 1));
        }
        return ev::undefined();
    });

    b.def("clearAllRoutes", 0, [](Value self, std::span<const Value>) {
        auto* h = hostModMatrixOf(self);
        if (h && h->matrix) h->matrix->clearAllRoutes();
        return ev::undefined();
    });

    b.def("setModWheel", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostModMatrixOf(self);
        if (h && h->matrix && !a.empty()) h->matrix->setModWheel(static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });

    b.def("setAftertouch", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostModMatrixOf(self);
        if (h && h->matrix && !a.empty()) h->matrix->setAftertouch(static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });

    b.accessor("routeCount", [](Value self, std::span<const Value>) {
        auto* h = hostModMatrixOf(self);
        return ev::fromDouble(h && h->matrix ? h->matrix->routeCount() : 0);
    }, nullptr);
}

static void decorateMidiInputProto(ObjectBuilder& b) {
    b.def("availablePorts", 0, [](Value self, std::span<const Value>) -> Value {
        auto* h = hostMidiInputOf(self);
        if (!h || !h->midi) return hostArrayOf(0, [](size_t) { return ev::undefined(); });
        auto ports = h->midi->availablePorts();
        return hostArrayOf(ports.size(), [&ports](size_t i) -> Value {
            ObjectBuilder p;
            p.set("index", ev::fromDouble(ports[i].index));
            p.set("name", ev::fromUtf8(ports[i].name));
            return p.get();
        });
    });

    b.def("open", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostMidiInputOf(self);
        if (h && h->midi && !a.empty()) return ev::fromBool(h->midi->open(i32At(a, 0)));
        return ev::fromBool(false);
    });

    b.def("close", 0, [](Value self, std::span<const Value>) {
        auto* h = hostMidiInputOf(self);
        if (h && h->midi) h->midi->close();
        return ev::undefined();
    });

    b.accessor("isOpen", [](Value self, std::span<const Value>) {
        auto* h = hostMidiInputOf(self);
        return ev::fromBool(h && h->midi ? h->midi->isOpen() : false);
    }, nullptr);

    b.def("connectToAllocator", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostMidiInputOf(self);
        if (h && h->midi && !a.empty()) {
            auto* alloc = hostVoiceAllocatorOf(a[0]);
            h->midi->connectToAllocator(alloc ? alloc->allocator.get() : nullptr);
        }
        return ev::undefined();
    });

    b.def("onControlChange", 2, [](Value self, std::span<const Value> a) {
        auto* h = hostMidiInputOf(self);
        if (h && h->midi && a.size() >= 2) {
            int cc = i32At(a, 0);
            if (cc >= 0 && cc < 128) {
                if (ev::isFunction(a[1])) {
                    h->ccCallbacks[cc] = ev::Persistent(a[1]);
                    h->midi->onControlChange(static_cast<uint8_t>(cc), [h, cc](uint8_t ch, uint8_t ccn, uint8_t val) {
                        if (h && ev::isFunction(h->ccCallbacks[cc].get())) {
                            Value args[3] = { ev::fromDouble(ch), ev::fromDouble(ccn), ev::fromDouble(val) };
                            ev::call(h->ccCallbacks[cc].get(), ev::undefined(), std::span<const Value>(args, 3));
                        }
                    });
                } else {
                    h->ccCallbacks[cc].set(ev::undefined());
                    h->midi->onControlChange(static_cast<uint8_t>(cc), nullptr);
                }
            }
        }
        return ev::undefined();
    });

    b.def("onPitchBend", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostMidiInputOf(self);
        if (h && h->midi && !a.empty()) {
            if (ev::isFunction(a[0])) {
                h->pitchBendCb = ev::Persistent(a[0]);
                h->midi->onPitchBend([h](uint8_t ch, int16_t val) {
                    if (h && ev::isFunction(h->pitchBendCb.get())) {
                        Value args[2] = { ev::fromDouble(ch), ev::fromDouble(val) };
                        ev::call(h->pitchBendCb.get(), ev::undefined(), std::span<const Value>(args, 2));
                    }
                });
            } else {
                h->pitchBendCb.set(ev::undefined());
                h->midi->onPitchBend(nullptr);
            }
        }
        return ev::undefined();
    });

    b.def("onRawEvent", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostMidiInputOf(self);
        if (h && h->midi && !a.empty()) {
            if (ev::isFunction(a[0])) {
                h->rawCb = ev::Persistent(a[0]);
                h->midi->onRawEvent([h](const broaudio::MidiEvent& ev) {
                    if (h && ev::isFunction(h->rawCb.get())) {
                        ObjectBuilder evObj;
                        evObj.set("type", ev::fromDouble(static_cast<int>(ev.type)));
                        evObj.set("channel", ev::fromDouble(ev.channel));
                        evObj.set("data1", ev::fromDouble(ev.data1));
                        evObj.set("data2", ev::fromDouble(ev.data2));
                        evObj.set("pitchBend", ev::fromDouble(ev.pitchBend));
                        evObj.set("timestamp", ev::fromDouble(ev.timestamp));
                        Value v = evObj.get();
                        ev::call(h->rawCb.get(), ev::undefined(), std::span<const Value>(&v, 1));
                    }
                });
            } else {
                h->rawCb.set(ev::undefined());
                h->midi->onRawEvent(nullptr);
            }
        }
        return ev::undefined();
    });

    b.def("processEvents", 0, [](Value self, std::span<const Value>) {
        auto* h = hostMidiInputOf(self);
        if (h && h->midi) h->midi->processEvents();
        return ev::undefined();
    });
}

Value makeVoiceAllocatorValue(int maxVoices) {
    auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
    if (!e) return ev::null();
    auto* h = new HostVoiceAllocator();
    h->allocator = std::make_unique<broaudio::VoiceAllocator>(*e, maxVoices);
    return g_voiceAllocatorClass.make(h, hostVoiceAllocatorDtor);
}

Value makeModMatrixValue() {
    auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
    if (!e) return ev::null();
    auto* h = new HostModMatrix();
    h->matrix = &e->modMatrix();
    return g_modMatrixClass.make(h, hostModMatrixDtor);
}

Value makeMidiInputValue() {
    auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
    if (!e) return ev::null();
    auto* h = new HostMidiInput();
    h->midi = std::make_unique<broaudio::MidiInput>(*e);
    return g_midiInputClass.make(h, hostMidiInputDtor);
}

Value makeMediaStreamValue() {
    auto* h = new HostMediaStream();
    return g_mediaStreamClass.make(h, hostMediaStreamDtor);
}

Value makeMediaStreamAudioSourceNodeValue() {
    auto* h = new HostMediaStreamAudioSourceNode();
    h->base.nodeType = AudioNodeType::Generic;
    return g_mediaStreamAudioSourceNodeClass.make(h, hostMediaStreamAudioSourceNodeDtor);
}

void registerAudioContextSynth(ObjectBuilder& b) {
    b.def("createVoiceAllocator", 1, [](Value, std::span<const Value> a) {
        int maxVoices = !a.empty() ? i32At(a, 0) : 16;
        return makeVoiceAllocatorValue(maxVoices);
    });

    b.def("getModMatrix", 0, [](Value, std::span<const Value>) {
        return makeModMatrixValue();
    });

    b.def("createMidiInput", 0, [](Value, std::span<const Value>) {
        return makeMidiInputValue();
    });

    b.def("createMediaStreamSource", 1, [](Value, std::span<const Value>) {
        return makeMediaStreamAudioSourceNodeValue();
    });

    b.def("createWavetable", 2, [](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::fromDouble(-1);
        const uint8_t* rawData = nullptr;
        size_t rawLen = 0, elemSize = 1;
        if (!bufferBytes(a[0], &rawData, &rawLen, &elemSize) || rawLen == 0) return ev::fromDouble(-1);
        int sr = a.size() >= 2 ? i32At(a, 1) : 44100;
        int count = static_cast<int>(rawLen / sizeof(float));
        auto bank = broaudio::WavetableBank::createFromWaveform(reinterpret_cast<const float*>(rawData), count, sr);
        return ev::fromDouble(registerWavetable(bank));
    });

    b.def("createWavetableFromWaveform", 3, [](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::fromDouble(-1);
        const uint8_t* rawData = nullptr;
        size_t rawLen = 0, elemSize = 1;
        if (!bufferBytes(a[0], &rawData, &rawLen, &elemSize) || rawLen == 0) return ev::fromDouble(-1);
        int count = a.size() >= 2 ? i32At(a, 1) : static_cast<int>(rawLen / sizeof(float));
        int sr = a.size() >= 3 ? i32At(a, 2) : 44100;
        auto bank = broaudio::WavetableBank::createFromWaveform(reinterpret_cast<const float*>(rawData), count, sr);
        return ev::fromDouble(registerWavetable(bank));
    });

    b.def("deleteWavetable", 1, [](Value, std::span<const Value> a) {
        if (!a.empty()) deleteWavetable(i32At(a, 0));
        return ev::undefined();
    });

    b.def("setVoiceWavetable", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) {
            int vId = i32At(a, 0);
            int wtId = i32At(a, 1);
            e->setVoiceWavetable(vId, findWavetable(wtId));
        }
        return ev::undefined();
    });

    b.def("getSpectrum", 1, [](Value, std::span<const Value> a) -> Value {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (!e || a.empty()) return ev::null();
        int bins = i32At(a, 0);
        if (bins <= 0) return ev::null();
        std::vector<float> spec = e->getSpectrum(bins);
        if (spec.empty()) return ev::null();
        Value arr = ev::createTypedArray(ev::elements::Float32, static_cast<uint32_t>(spec.size()));
        ev::fillTypedArray(arr, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(spec.data()), spec.size() * sizeof(float)));
        return arr;
    });

    b.def("renderBlock", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && !a.empty()) e->renderBlock(i32At(a, 0));
        return ev::undefined();
    });

    b.def("voicePresetToJson", 1, [](Value, std::span<const Value> a) {
        if (a.empty()) return ev::fromUtf8("{}");
        return ev::fromUtf8(broaudio::toJson(broaudio::voicePresetFromJson(ev::toUtf8(a[0]))));
    });

    b.def("busPresetToJson", 1, [](Value, std::span<const Value> a) {
        if (a.empty()) return ev::fromUtf8("{}");
        return ev::fromUtf8(broaudio::toJson(broaudio::busPresetFromJson(ev::toUtf8(a[0]))));
    });

    b.def("modPresetToJson", 1, [](Value, std::span<const Value> a) {
        if (a.empty()) return ev::fromUtf8("{}");
        return ev::fromUtf8(broaudio::toJson(broaudio::modPresetFromJson(ev::toUtf8(a[0]))));
    });

    b.def("enginePresetToJson", 1, [](Value, std::span<const Value> a) {
        if (a.empty()) return ev::fromUtf8("{}");
        return ev::fromUtf8(broaudio::toJson(broaudio::enginePresetFromJson(ev::toUtf8(a[0]))));
    });

    b.def("applyVoicePreset", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) {
            e->applyVoicePreset(i32At(a, 0), broaudio::voicePresetFromJson(ev::toUtf8(a[1])));
        }
        return ev::undefined();
    });

    b.def("applyBusPreset", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) {
            e->applyBusPreset(i32At(a, 0), broaudio::busPresetFromJson(ev::toUtf8(a[1])));
        }
        return ev::undefined();
    });

    b.def("applyModPreset", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && !a.empty()) {
            e->applyModPreset(broaudio::modPresetFromJson(ev::toUtf8(a[0])));
        }
        return ev::undefined();
    });

    b.def("applyEnginePreset", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && !a.empty()) {
            e->applyEnginePreset(broaudio::enginePresetFromJson(ev::toUtf8(a[0])));
        }
        return ev::undefined();
    });

    b.def("savePreset", 2, [](Value, std::span<const Value> a) {
        if (a.size() < 2) return ev::fromBool(false);
        std::string json = ev::toUtf8(a[0]);
        std::string path = bro::util::resolveAssetWritePath(ev::toUtf8(a[1]));
        return ev::fromBool(broaudio::savePresetToFile(json, path.c_str()));
    });

    b.def("loadPreset", 1, [](Value, std::span<const Value> a) {
        if (a.empty()) return ev::fromUtf8("");
        std::string path = bro::util::resolveAssetPath(ev::toUtf8(a[0]));
        return ev::fromUtf8(broaudio::loadPresetFromFile(path.c_str()));
    });
}

void installAudioSynthGlobals() {
    g_voiceAllocatorClass.install("VoiceAllocator", 0, nullptr, decorateVoiceAllocatorProto);
    g_modMatrixClass.install("ModMatrix", 0, nullptr, decorateModMatrixProto);
    g_midiInputClass.install("MidiInput", 0, nullptr, decorateMidiInputProto);
    g_mediaStreamClass.install("MediaStream", 0, nullptr, nullptr);
    g_mediaStreamAudioSourceNodeClass.install("MediaStreamAudioSourceNode", 0, nullptr, nullptr);
    g_mediaStreamAudioSourceNodeClass.inherit(g_audioNodeClass);
}

}  // namespace bro::bronze_host
