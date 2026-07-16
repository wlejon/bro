#pragma once

struct JSContext;

namespace bro::engine { class Engine; }

namespace bro::js {

/// bro.time — global pause + timescale over the engine's scaled clock
/// (Godot Engine.time_scale / SceneTree.paused analog).
///
///   bro.time.scale   — get/set time multiplier, clamped to [0, 100]
///   bro.time.paused  — get/set global pause (effective scale 0 + audio
///                      output suspended + rAF callbacks skipped)
///   bro.time.now     — read-only current scaled engine time in ms
///
/// Holds no JSValues — just a stashed Engine*, same pattern as bro.menu.
class TimeBindings {
public:
    static void install(JSContext* ctx, engine::Engine* engine);
};

} // namespace bro::js
