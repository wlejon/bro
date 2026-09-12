#include "bronze_host/host_audio_internal.h"

namespace bro::bronze_host {

HostClass g_sequenceClass;

void hostSequenceDtor(void* p) {
    auto* h = static_cast<HostSequence*>(p);
    for (auto& cb : h->automationCallbacks) cb.set(ev::undefined());
    delete h;
}

HostSequence* hostSequenceOf(Value v) {
    auto* h = static_cast<HostSequence*>(ev::handleData(v));
    return (h && h->tag == kHostSequenceTag) ? h : nullptr;
}

static broaudio::InterpMode parseInterpMode(const std::string& str) {
    if (str == "step") return broaudio::InterpMode::Step;
    if (str == "smooth") return broaudio::InterpMode::Smooth;
    return broaudio::InterpMode::Linear;
}

static const char* interpModeToString(broaudio::InterpMode mode) {
    switch (mode) {
        case broaudio::InterpMode::Step: return "step";
        case broaudio::InterpMode::Smooth: return "smooth";
        default: return "linear";
    }
}

static void decorateSequenceProto(ObjectBuilder& b) {
    b.def("setBPM", 2, [](Value self, std::span<const Value> a) {
        auto* h = hostSequenceOf(self);
        if (h && h->seq && !a.empty()) {
            if (a.size() >= 2) h->seq->setBPM(numAt(a, 0), numAt(a, 1));
            else h->seq->setBPM(numAt(a, 0));
        }
        return ev::undefined();
    });

    b.accessor("bpm", [](Value self, std::span<const Value>) {
        auto* h = hostSequenceOf(self);
        return ev::fromDouble(h && h->seq ? h->seq->bpm() : 120.0);
    }, nullptr);

    b.def("setTimeSignature", 2, [](Value self, std::span<const Value> a) {
        auto* h = hostSequenceOf(self);
        if (h && h->seq && a.size() >= 2) h->seq->setTimeSignature(i32At(a, 0), i32At(a, 1));
        return ev::undefined();
    });

    b.def("addNote", 4, [](Value self, std::span<const Value> a) {
        auto* h = hostSequenceOf(self);
        if (h && h->seq && a.size() >= 4) {
            broaudio::NoteEvent ev;
            ev.beatPosition = numAt(a, 0);
            ev.note = i32At(a, 1);
            ev.velocity = static_cast<float>(numAt(a, 2));
            ev.duration = numAt(a, 3);
            h->seq->addNote(ev);
        }
        return ev::undefined();
    });

    b.def("removeNote", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostSequenceOf(self);
        if (h && h->seq && !a.empty()) h->seq->removeNote(i32At(a, 0));
        return ev::undefined();
    });

    b.def("clearNotes", 0, [](Value self, std::span<const Value>) {
        auto* h = hostSequenceOf(self);
        if (h && h->seq) h->seq->clearNotes();
        return ev::undefined();
    });

    b.accessor("noteCount", [](Value self, std::span<const Value>) {
        auto* h = hostSequenceOf(self);
        return ev::fromDouble(h && h->seq ? h->seq->noteCount() : 0);
    }, nullptr);

    b.def("note", 1, [](Value self, std::span<const Value> a) -> Value {
        auto* h = hostSequenceOf(self);
        if (!h || !h->seq || a.empty()) return ev::null();
        int idx = i32At(a, 0);
        if (idx < 0 || idx >= h->seq->noteCount()) return ev::null();
        const auto& ev = h->seq->note(idx);
        ObjectBuilder obj;
        obj.set("beatPosition", ev::fromDouble(ev.beatPosition));
        obj.set("note", ev::fromDouble(ev.note));
        obj.set("velocity", ev::fromDouble(ev.velocity));
        obj.set("duration", ev::fromDouble(ev.duration));
        return obj.get();
    });

    b.def("play", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostSequenceOf(self);
        if (h && h->seq) {
            double when = !a.empty() ? numAt(a, 0) : (hostEngine() && hostEngine()->audioEngine() ? hostEngine()->audioEngine()->currentTime() : 0.0);
            h->seq->play(when);
        }
        return ev::undefined();
    });

    b.def("stop", 0, [](Value self, std::span<const Value>) {
        auto* h = hostSequenceOf(self);
        if (h && h->seq) h->seq->stop();
        return ev::undefined();
    });

    b.def("pause", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostSequenceOf(self);
        if (h && h->seq) {
            double when = !a.empty() ? numAt(a, 0) : (hostEngine() && hostEngine()->audioEngine() ? hostEngine()->audioEngine()->currentTime() : 0.0);
            h->seq->pause(when);
        }
        return ev::undefined();
    });

    b.def("resume", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostSequenceOf(self);
        if (h && h->seq) {
            double when = !a.empty() ? numAt(a, 0) : (hostEngine() && hostEngine()->audioEngine() ? hostEngine()->audioEngine()->currentTime() : 0.0);
            h->seq->resume(when);
        }
        return ev::undefined();
    });

    b.accessor("playing", [](Value self, std::span<const Value>) {
        auto* h = hostSequenceOf(self);
        return ev::fromBool(h && h->seq ? h->seq->isPlaying() : false);
    }, nullptr);

    b.accessor("paused", [](Value self, std::span<const Value>) {
        auto* h = hostSequenceOf(self);
        return ev::fromBool(h && h->seq ? h->seq->isPaused() : false);
    }, nullptr);

    b.def("setLoopEnabled", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostSequenceOf(self);
        if (h && h->seq && !a.empty()) h->seq->setLoopEnabled(boolAt(a, 0));
        return ev::undefined();
    });

    b.accessor("loopEnabled", [](Value self, std::span<const Value>) {
        auto* h = hostSequenceOf(self);
        return ev::fromBool(h && h->seq ? h->seq->isLoopEnabled() : false);
    }, nullptr);

    b.def("setLoopRange", 2, [](Value self, std::span<const Value> a) {
        auto* h = hostSequenceOf(self);
        if (h && h->seq && a.size() >= 2) h->seq->setLoopRange(numAt(a, 0), numAt(a, 1));
        return ev::undefined();
    });

    b.def("currentBeat", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostSequenceOf(self);
        if (!h || !h->seq) return ev::fromDouble(0.0);
        double when = !a.empty() ? numAt(a, 0) : (hostEngine() && hostEngine()->audioEngine() ? hostEngine()->audioEngine()->currentTime() : 0.0);
        return ev::fromDouble(h->seq->currentBeat(when));
    });

    b.def("update", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostSequenceOf(self);
        if (h && h->seq) {
            double when = !a.empty() ? numAt(a, 0) : (hostEngine() && hostEngine()->audioEngine() ? hostEngine()->audioEngine()->currentTime() : 0.0);
            h->seq->update(when);
        }
        return ev::undefined();
    });

    b.def("addAutomationLane", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostSequenceOf(self);
        if (!h || !h->seq || a.empty() || !ev::isFunction(a[0])) return ev::fromDouble(-1);
        int laneIdx = h->seq->automationLaneCount();
        h->automationCallbacks.emplace_back(a[0]);
        size_t cbIdx = h->automationCallbacks.size() - 1;
        h->seq->addAutomationLane([h, cbIdx](float val) {
            if (h && cbIdx < h->automationCallbacks.size() && ev::isFunction(h->automationCallbacks[cbIdx].get())) {
                Value arg = ev::fromDouble(val);
                ev::call(h->automationCallbacks[cbIdx].get(), ev::undefined(), std::span<const Value>(&arg, 1));
            }
        });
        return ev::fromDouble(laneIdx);
    });

    b.def("removeAutomationLane", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostSequenceOf(self);
        if (h && h->seq && !a.empty()) {
            int idx = i32At(a, 0);
            if (idx >= 0 && idx < h->seq->automationLaneCount()) {
                h->seq->removeAutomationLane(idx);
                if (idx < static_cast<int>(h->automationCallbacks.size())) {
                    h->automationCallbacks.erase(h->automationCallbacks.begin() + idx);
                }
            }
        }
        return ev::undefined();
    });

    b.def("clearAutomationLanes", 0, [](Value self, std::span<const Value>) {
        auto* h = hostSequenceOf(self);
        if (h && h->seq) {
            h->seq->clearAutomationLanes();
            h->automationCallbacks.clear();
        }
        return ev::undefined();
    });

    b.accessor("automationLaneCount", [](Value self, std::span<const Value>) {
        auto* h = hostSequenceOf(self);
        return ev::fromDouble(h && h->seq ? h->seq->automationLaneCount() : 0);
    }, nullptr);

    b.def("addAutomationPoint", 3, [](Value self, std::span<const Value> a) {
        auto* h = hostSequenceOf(self);
        if (h && h->seq && a.size() >= 3) {
            int lane = i32At(a, 0);
            if (lane >= 0 && lane < h->seq->automationLaneCount()) {
                h->seq->automationLane(lane).addPoint(numAt(a, 1), static_cast<float>(numAt(a, 2)));
            }
        }
        return ev::undefined();
    });

    b.def("removeAutomationPoint", 2, [](Value self, std::span<const Value> a) {
        auto* h = hostSequenceOf(self);
        if (h && h->seq && a.size() >= 2) {
            int lane = i32At(a, 0);
            if (lane >= 0 && lane < h->seq->automationLaneCount()) {
                h->seq->automationLane(lane).removePoint(i32At(a, 1));
            }
        }
        return ev::undefined();
    });

    b.def("clearAutomationPoints", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostSequenceOf(self);
        if (h && h->seq && !a.empty()) {
            int lane = i32At(a, 0);
            if (lane >= 0 && lane < h->seq->automationLaneCount()) {
                h->seq->automationLane(lane).clearPoints();
            }
        }
        return ev::undefined();
    });

    b.def("setAutomationInterpMode", 2, [](Value self, std::span<const Value> a) {
        auto* h = hostSequenceOf(self);
        if (h && h->seq && a.size() >= 2) {
            int lane = i32At(a, 0);
            if (lane >= 0 && lane < h->seq->automationLaneCount()) {
                h->seq->automationLane(lane).setInterpMode(parseInterpMode(ev::toUtf8(a[1])));
            }
        }
        return ev::undefined();
    });

    b.def("automationPointCount", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostSequenceOf(self);
        if (h && h->seq && !a.empty()) {
            int lane = i32At(a, 0);
            if (lane >= 0 && lane < h->seq->automationLaneCount()) {
                return ev::fromDouble(h->seq->automationLane(lane).pointCount());
            }
        }
        return ev::fromDouble(0);
    });

    b.def("automationPoint", 2, [](Value self, std::span<const Value> a) -> Value {
        auto* h = hostSequenceOf(self);
        if (!h || !h->seq || a.size() < 2) return ev::null();
        int lane = i32At(a, 0);
        int ptIdx = i32At(a, 1);
        if (lane < 0 || lane >= h->seq->automationLaneCount() || ptIdx < 0 || ptIdx >= h->seq->automationLane(lane).pointCount()) return ev::null();
        const auto& pt = h->seq->automationLane(lane).point(ptIdx);
        ObjectBuilder obj;
        obj.set("beat", ev::fromDouble(pt.beat));
        obj.set("value", ev::fromDouble(pt.value));
        return obj.get();
    });

    b.def("automationInterpMode", 1, [](Value self, std::span<const Value> a) {
        auto* h = hostSequenceOf(self);
        if (h && h->seq && !a.empty()) {
            int lane = i32At(a, 0);
            if (lane >= 0 && lane < h->seq->automationLaneCount()) {
                return ev::fromUtf8(interpModeToString(h->seq->automationLane(lane).interpMode()));
            }
        }
        return ev::fromUtf8("linear");
    });
}

Value makeSequenceValue(HostVoiceAllocator* va) {
    if (!va || !va->allocator) return ev::null();
    auto* h = new HostSequence();
    h->seq = std::make_unique<broaudio::Sequence>(*va->allocator);
    return g_sequenceClass.make(h, hostSequenceDtor);
}

void registerAudioContextSequencer(ObjectBuilder& b) {
    b.def("createSequence", 1, [](Value, std::span<const Value> a) {
        HostVoiceAllocator* va = !a.empty() ? hostVoiceAllocatorOf(a[0]) : nullptr;
        return makeSequenceValue(va);
    });
}

void installAudioSequencerGlobals() {
    g_sequenceClass.install("Sequence", 0, nullptr, decorateSequenceProto);
}

}  // namespace bro::bronze_host
