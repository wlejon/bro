// Feature stubs for the optional subsystems (AI tower + Tier-1 renderer/
// service features).
//
// Each block below is compiled only when its BRO_WITH_* flag is OFF, and
// defines the same install entry point that the (now empty) wrapped
// *_bindings.cpp would have defined. The stub installs an unavailable
// `bro.<name>` namespace so apps can feature-detect (`bro.lm.available`) and
// get a clear "compiled without BRO_WITH_X" error instead of a bare
// ReferenceError. See docs/build-options.md.
//
// This TU is always compiled into bro_js; the #if !BRO_WITH_X guards make each
// stub active only in the configuration where the real definition is absent,
// so there is never a duplicate-symbol clash.

#include "js/feature_stub.h"

// Thin binding headers (install-fn declarations + forward decls only).
// installTensorBindings is declared in ai_bindings.h (which only forward-
// declares brotensor/brogameagent types — no sibling headers, so it is safe to
// include in a stubbed build).
#include "js/gpu_bindings.h"
#include "js/ai_bindings.h"
#include "js/lm_bindings.h"
#include "js/diffusion_bindings.h"
#include "js/vision_bindings.h"
#include "js/stt_bindings.h"
#include "js/tts_bindings.h"
#include "js/diar_bindings.h"
#include "js/rave_bindings.h"
#include "js/wake_bindings.h"
#include "js/kws_bindings.h"
#include "js/sense_bindings.h"
#include "js/gesture_bindings.h"
#include "js/listen_bindings.h"
#include "js/triposplat_bindings.h"
#include "js/motion_bindings.h"

// Tier-1 binding headers (install-fn declarations + forward decls only — all
// stub-safe: no sibling headers pulled in).
#include "js/flora_bindings.h"
#include "js/net_bindings.h"
#include "js/video_bindings.h"
#include "js/mesh_bindings.h"

// Forward decls for the audio-taking soundml install signatures. (listen_host's
// entry points are guarded directly in the engine — they use the fat
// listen_host.h header — so no stub is needed for them here.)
namespace broaudio { class Engine; }
namespace bro::engine { class AudioInference; }

namespace bro::js {

// ── TENSOR (bro.tensor + bro.gpu) ────────────────────────────────────────────
#if !BRO_WITH_TENSOR
void installTensorBindings(JSContext* ctx) {
    installUnavailableNamespace(ctx, "tensor", "BRO_WITH_TENSOR");
}
void installGpuBindings(JSContext* ctx) {
    // bro.gpu is the always-present runtime backend probe. With no GPU tensor
    // backend built, make the stub behaviorally identical to a real CPU-only
    // binding rather than a throwing proxy or a half-populated object, so any
    // app written against the real surface (see system/skeletons/ai) works
    // verbatim: cpu is the sole registered device (and thus the default
    // backend), and the method surface exists, mirroring the real binding's
    // CPU-path answers — no card name, no mem info, nothing to trim.
    installFeatureStub(ctx,
        "(function(){var b=(globalThis.bro=globalThis.bro||{});"
        "b.gpu={available:false,backend:'cpu',devices:['cpu'],compiledBackends:['cpu'],"
        "deviceName:function(){return null;},"
        "deviceCount:function(d){return (!d||d==='cpu')?1:0;},"
        "memoryInfo:function(){return null;},"
        "trim:function(){return false;}};})();");
}
#endif

// ── LM ───────────────────────────────────────────────────────────────────────
#if !BRO_WITH_LM
void installLmBindings(JSContext* ctx) {
    installUnavailableNamespace(ctx, "lm", "BRO_WITH_LM");
}
#endif

// ── DIFFUSION ────────────────────────────────────────────────────────────────
#if !BRO_WITH_DIFFUSION
void installDiffusionBindings(JSContext* ctx) {
    installUnavailableNamespace(ctx, "diffusion", "BRO_WITH_DIFFUSION");
}
#endif

// ── VISION ───────────────────────────────────────────────────────────────────
#if !BRO_WITH_VISION
void installVisionBindings(JSContext* ctx) {
    installUnavailableNamespace(ctx, "vision", "BRO_WITH_VISION");
}
#endif

// ── SOUNDML (stt/tts/diar/rave/wake/kws/sense/gesture/listen) ─────────────────
#if !BRO_WITH_SOUNDML
void installSttBindings(JSContext* ctx)  { installUnavailableNamespace(ctx, "stt",  "BRO_WITH_SOUNDML"); }
void installTtsBindings(JSContext* ctx)  { installUnavailableNamespace(ctx, "tts",  "BRO_WITH_SOUNDML"); }
void installDiarBindings(JSContext* ctx) { installUnavailableNamespace(ctx, "diar", "BRO_WITH_SOUNDML"); }
void installRaveBindings(JSContext* ctx) { installUnavailableNamespace(ctx, "rave", "BRO_WITH_SOUNDML"); }
void installWakeBindings(JSContext* ctx, broaudio::Engine*, engine::AudioInference*) {
    installUnavailableNamespace(ctx, "wake", "BRO_WITH_SOUNDML");
}
void installKwsBindings(JSContext* ctx, broaudio::Engine*, engine::AudioInference*) {
    installUnavailableNamespace(ctx, "kws", "BRO_WITH_SOUNDML");
}
void installSenseBindings(JSContext* ctx, broaudio::Engine*, engine::AudioInference*) {
    installUnavailableNamespace(ctx, "sense", "BRO_WITH_SOUNDML");
}
void installGestureBindings(JSContext* ctx, broaudio::Engine*, engine::AudioInference*) {
    installUnavailableNamespace(ctx, "gesture", "BRO_WITH_SOUNDML");
}
void installListenBindings(JSContext* ctx) {
    installUnavailableNamespace(ctx, "listen", "BRO_WITH_SOUNDML");
}
#endif

// ── FLORA (bro.flora) ────────────────────────────────────────────────────────
#if !BRO_WITH_FLORA
void FloraBindings::install(JSContext* ctx) {
    installUnavailableNamespace(ctx, "flora", "BRO_WITH_FLORA");
}
#endif

// ── NET (bro.net) ────────────────────────────────────────────────────────────
// install() installs the unavailable bro.net namespace; poll/cleanup are
// no-ops so the engine main loop and worker plumbing (worker.cpp holds a
// null NetService*) link without guarding every call site. The service pointer
// is always null in this build, so poll/cleanup are never reached at runtime.
#if !BRO_WITH_NET
void NetBindings::install(JSContext* ctx, net::NetService* /*service*/) {
    installUnavailableNamespace(ctx, "net", "BRO_WITH_NET");
}
void NetBindings::poll(JSContext* /*ctx*/) {}
bool NetBindings::hasActivity(JSContext* /*ctx*/) { return false; }
void NetBindings::cleanup(JSContext* /*ctx*/) {}
#endif

// ── VIDEO (VideoEncoder / GifEncoder) ────────────────────────────────────────
// No bro.video namespace to stub — these are global encoder classes. With video
// off they're simply not installed; advanced apps feature-detect via
// `typeof VideoEncoder !== 'undefined'`. (<video> playback degrades separately
// to an inert black box in el_video.cpp.)
#if !BRO_WITH_VIDEO
void VideoBindings::install(JSContext* /*ctx*/, const std::string& /*basePath*/) {}
#endif

// ── GAMEAI (bro.ai — game-agent core: navgrid/pathfinding/steering/MCTS) ─────
// install() installs the unavailable bro.ai namespace so apps feature-detect
// (`bro.ai.available === false`); cleanup is a no-op. The real definitions live
// in ai_bindings.cpp (guarded by BRO_WITH_GAMEAI). ai_bindings.h is stub-safe
// (forward-decls brogameagent types only). 3D forces GAMEAI on, so when this
// stub is active the whole scene graph is compiled out too.
#if !BRO_WITH_GAMEAI
void AIBindings::install(JSContext* ctx) {
    installUnavailableNamespace(ctx, "ai", "BRO_WITH_GAMEAI");
}
void AIBindings::cleanup(JSContext* /*ctx*/) {}
#endif

// ── MESH (global Mesh class — 3D-only) ───────────────────────────────────────
// worker.cpp installs the Mesh class into every worker context unconditionally;
// with 3D off there is no bromesh, so install/cleanup are no-ops (the Mesh class
// is simply absent). The data accessors (classId/getMeshData/wrapMeshData/…) are
// only called from 3D code paths, which are compiled out, so they need no stub.
#if !BRO_WITH_3D
void MeshBindings::install(JSContext* /*ctx*/) {}
void MeshBindings::cleanup(JSContext* /*ctx*/) {}
#endif

// ── TRIPOSPLAT ───────────────────────────────────────────────────────────────
#if !BRO_WITH_TRIPOSPLAT
void installTriposplatBindings(JSContext* ctx) {
    installUnavailableNamespace(ctx, "triposplat", "BRO_WITH_TRIPOSPLAT");
}
#endif

// ── MOTION (ARDY text-to-motion — needs both the denoiser (DIFFUSION) and the
//    LLM2Vec text encoder (LM)) ─────────────────────────────────────────────
#if !(BRO_WITH_DIFFUSION && BRO_WITH_LM)
void installMotionBindings(JSContext* ctx) {
    installUnavailableNamespace(ctx, "motion", "BRO_WITH_DIFFUSION+BRO_WITH_LM");
}
#endif

// ── GAMEAI_NN (brogameagent nn/learn) ────────────────────────────────────────
// The GAMEAI core bindings (ai_bindings/ai_belief/ai_parallel/ai_generic_mcts)
// call these three helpers to check whether a JS value is a *neural* prior /
// evaluator / inference backend. With the neural layer off, no such wrappers
// exist, so returning empty/null is the correct answer.
#if !BRO_WITH_GAMEAI_NN
std::shared_ptr<brogameagent::mcts::IPrior>
extractPriorShared(JSContext* /*ctx*/, JSValueConst /*v*/) { return {}; }
std::shared_ptr<brogameagent::mcts::IEvaluator>
extractHeroEvaluatorShared(JSContext* /*ctx*/, JSValueConst /*v*/) { return {}; }
brogameagent::learn::IInferenceBackend*
inferenceBackendFromJS(JSContext* /*ctx*/, JSValueConst /*v*/) { return nullptr; }
#endif

} // namespace bro::js
