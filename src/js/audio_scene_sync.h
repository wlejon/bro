#pragma once

// Scene-attached audio emitters + camera-bound listener — the engine-side
// auto-sync that replaces per-frame JS pushes of spatial audio positions
// (the AudioStreamPlayer3D analog). JS attaches a broaudio playback or voice
// handle to a scene node (node.attachAudioEmitter) or binds the audio
// listener to a scene's camera (scene.bindAudioListenerToCamera); each engine
// frame syncAudioSceneEmitters() pushes world positions — and finite-
// difference velocities feeding the Doppler model — into broaudio's atomic
// spatial parameters. Zero per-frame JS.
//
// Threading: everything here is main-thread control plane. The registry is a
// plain vector of {weak LivenessToken, node id, handle} entries — the same
// wrapper-liveness discipline as scene_bindings_internal.h, so destroyed
// nodes/graphs self-prune and finished/stopped broaudio handles degrade to
// no-op atomic stores (broaudio's setters ignore dead ids).
//
// All functions are compiled to no-ops when BRO_WITH_3D is off.

namespace broaudio { class Engine; }

namespace bro::js {

// Wire the registry to the engine's broaudio instance (engine_init) /
// clear it before the audio engine dies (engine_lifecycle).
void installAudioSceneSync(broaudio::Engine* engine);
void shutdownAudioSceneSync();

// Per-frame push, called right after SceneGraph::tickAnimations in both the
// windowed frame and headless advanceTime paths (so tween/animation motion
// this frame is what the audio hears). `dtSec` is the scaled frame dt
// (bro.time); velocities are only re-derived when dtSec > 0.
void syncAudioSceneEmitters(float dtSec);

} // namespace bro::js
