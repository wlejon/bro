#include "scene/animation_player.h"
#include "util/log.h"

#include <algorithm>

namespace bro::scene {

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------

bool AnimationPlayer::setStateMachine(StateMachineDef def, std::string* err) {
    auto fail = [&](const std::string& msg) {
        if (err) *err = msg;
        return false;
    };
    if (!skeleton_) return fail("setSkeleton first");
    if (def.states.empty()) return fail("needs at least one state");

    std::vector<MachineState> states;
    states.reserve(def.states.size());
    for (const auto& s : def.states) {
        if (s.name.empty() || s.name == "*")
            return fail("state names must be non-empty and not '*'");
        for (const auto& prev : states)
            if (prev.name == s.name)
                return fail("duplicate state '" + s.name + "'");
        if (!hasBlendSpace(s.source) && !hasClip(s.source))
            return fail("state '" + s.name + "': source '" + s.source +
                        "' is not a registered clip or blend space");
        states.push_back({s.name, s.source, s.speed, s.loop});
    }
    auto stateIndex = [&](const std::string& name) -> int {
        for (size_t i = 0; i < states.size(); ++i)
            if (states[i].name == name) return (int)i;
        return -1;
    };

    std::vector<MachineTransition> transitions;
    transitions.reserve(def.transitions.size());
    for (const auto& t : def.transitions) {
        MachineTransition mt;
        mt.from = -1;
        if (t.from != "*") {
            mt.from = stateIndex(t.from);
            if (mt.from < 0)
                return fail("transition from unknown state '" + t.from + "'");
        }
        mt.to = stateIndex(t.to);
        if (mt.to < 0)
            return fail("transition to unknown state '" + t.to + "'");
        mt.fade = std::max(t.fade, 0.0f);
        mt.autoAdvance = t.autoAdvance;
        mt.syncPhase = t.syncPhase;
        transitions.push_back(mt);
    }

    int initial = def.initial.empty() ? 0 : stateIndex(def.initial);
    if (initial < 0)
        return fail("unknown initial state '" + def.initial + "'");

    machineStates_ = std::move(states);
    machineTransitions_ = std::move(transitions);
    machineCurrent_ = -1;
    // Enter the initial state immediately, without firing onStateChanged
    // (the app installs the callback around the same time; the initial
    // entry is not a transition).
    enterState(initial, 0.0f, /*syncPhase=*/false, /*fireCallback=*/false);
    return true;
}

const std::string& AnimationPlayer::currentState() const {
    static const std::string kNone;
    return machineCurrent_ >= 0 ? machineStates_[machineCurrent_].name : kNone;
}

int AnimationPlayer::findState(const std::string& name) const {
    for (size_t i = 0; i < machineStates_.size(); ++i)
        if (machineStates_[i].name == name) return (int)i;
    return -1;
}

int AnimationPlayer::findTransition(int from, int to) const {
    for (size_t i = 0; i < machineTransitions_.size(); ++i)
        if (machineTransitions_[i].from == from &&
            machineTransitions_[i].to == to && from >= 0)
            return (int)i;
    for (size_t i = 0; i < machineTransitions_.size(); ++i)
        if (machineTransitions_[i].from == -1 &&
            machineTransitions_[i].to == to)
            return (int)i;
    return -1;
}

int AnimationPlayer::findAutoTransition(int from) const {
    for (size_t i = 0; i < machineTransitions_.size(); ++i)
        if (machineTransitions_[i].autoAdvance &&
            machineTransitions_[i].from == from && from >= 0)
            return (int)i;
    for (size_t i = 0; i < machineTransitions_.size(); ++i)
        if (machineTransitions_[i].autoAdvance &&
            machineTransitions_[i].from == -1)
            return (int)i;
    return -1;
}

bool AnimationPlayer::travel(const std::string& stateName) {
    if (machineStates_.empty()) return false;
    int idx = findState(stateName);
    if (idx < 0) return false;
    if (idx == machineCurrent_) return true;   // already there: no-op

    int tr = findTransition(machineCurrent_, idx);
    float fade = 0.0f;
    bool sync = false;
    if (tr >= 0) {
        fade = machineTransitions_[tr].fade;
        sync = machineTransitions_[tr].syncPhase;
    } else {
        LOG_WARN("AnimationPlayer::travel: no transition '%s' -> '%s'; "
                 "switching directly (fade 0)",
                 machineCurrent_ >= 0
                     ? machineStates_[machineCurrent_].name.c_str()
                     : "(suspended)",
                 stateName.c_str());
    }
    enterState(idx, fade, sync, /*fireCallback=*/true);
    return true;
}

void AnimationPlayer::enterState(int idx, float fade, bool syncPhase,
                                 bool fireCallback) {
    const int fromIdx = machineCurrent_;
    const MachineState& st = machineStates_[idx];

    // Phase carry-over source: the outgoing base track, if it is a cycle
    // (a blend space, or a looping clip).
    float oldPhase = 0.0f;
    bool oldCyclic = false;
    if (base_.valid()) {
        if (base_.space) {
            oldPhase = base_.phase;
            oldCyclic = true;
        } else if (base_.clip && base_.clip->duration > 0.0f) {
            oldPhase = base_.time / base_.clip->duration;
            oldCyclic = base_.loop;
        }
    }

    PlayOptions opts;
    opts.loop = st.loop;
    opts.speed = st.speed;
    opts.fadeTime = fade;
    if (!startBase(st.source, opts)) {
        LOG_WARN("AnimationPlayer: state '%s' source '%s' vanished",
                 st.name.c_str(), st.source.c_str());
        return;
    }
    machineCurrent_ = idx;

    // syncPhase: both states must be cycles — the incoming track starts at
    // the outgoing phase so gait cycles stay foot-aligned across the switch.
    if (syncPhase && oldCyclic) {
        if (base_.space) {
            base_.updateBlendWeights();
            base_.phase = oldPhase;
            base_.time = base_.phase * base_.cachedCycleDur;
            applyPose();
        } else if (base_.clip && base_.loop && base_.clip->duration > 0.0f) {
            base_.time = oldPhase * base_.clip->duration;
            applyPose();
        }
    }

    if (fireCallback && onStateChanged_) {
        static const std::string kNone;
        onStateChanged_(fromIdx >= 0 ? machineStates_[fromIdx].name : kNone,
                        st.name);
    }
}

} // namespace bro::scene
