#pragma once

// bro's half of `bro.vision`.
//
// brovisionml ships the whole model surface (brovisionml_api, installed once
// per realm by host_sibling_apis.cpp), but two things about the old QuickJS
// binding could never live in a standalone sibling, and both are restored
// here, on top of the sibling rather than instead of it:
//
//   1. ImageBitmaps. The old binding returned `image` (and BiRefNet's
//      `matte`) as engine-minted ImageBitmaps a program could hand straight
//      to drawImage/texImage2D. An ImageBitmap is an SkImage behind a bro
//      host class (host_imagebitmap.cpp), so the sibling returns the pixel
//      planes as typed arrays and says in as many words that the caller
//      rasterizes. bro is that caller. Every wrapped op keeps the sibling's
//      typed-array fields AND adds the bitmaps back.
//
//   2. Async. A depth pass or a SAM everything-sweep is tens to hundreds of
//      milliseconds; running it inside the JS call drops frames. The old
//      binding took an `onDone` callback, ran the model on a worker thread
//      and delivered the result on the JS thread from the engine's frame
//      pump, returning an AsyncHandle with `.cancel()`. A sibling that owns
//      no event loop cannot do that either.
//
// WHY THE OPS ARE RE-DRIVEN HERE rather than wrapped around a call to the
// sibling's own method. The compute has to happen off the JS thread, and a
// bronze realm belongs to exactly one thread — calling the sibling's native
// method from a worker would race the main thread's heap. So the split is
// the one the old binding used: parse and validate on the JS thread, run the
// model (plain C++, no Values) on the worker, build the result on the JS
// thread. The synchronous path runs the SAME compute and the SAME builder
// inline, so `op(img)` and `op(img, {onDone})` cannot disagree — that is
// what tests/vision/vision_async.js asserts.
//
// Everything the compute needs comes from brovisionml's own api headers
// (the wrapper structs, the image reader, the typed-array makers,
// buildSegmentation), so the model calls and the result shapes have one
// source of truth, not two.

#if BRO_WITH_VISION

#include "embed/embed.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bro::engine {
class Engine;
}

namespace bro::bronze_host {

namespace ev = bronze::embed;
using Value = bronze::Value;

// ---------------------------------------------------------------------------
// Install / pump (host_vision_jobs.cpp)
// ---------------------------------------------------------------------------

// Re-decorate brovisionml's prototypes with the ImageBitmap-carrying,
// optionally-async bodies. Called from installSiblingApis AFTER
// brovisionml::api::installVision() has minted the classes, once per realm.
// `engine` may be null (worker realms): it is only used to register the
// per-frame drain and the shutdown hook, and only the first time.
void installVisionHostOps(engine::Engine* engine);

// Finish the jobs launched from THIS thread whose worker has returned: join,
// build the result, fire onDone. Cheap no-op when nothing is in flight.
// Registered on the engine's frame pump for the main realm and called from
// tickWorkerSiblingApis for a Worker.
void tickVisionJobs();

// Cancel and join every in-flight job on this thread, then run its
// completion so the rooted callbacks are released. Called from the engine's
// shutdown hook before the runtime goes away.
void shutdownVisionJobs();

// ---------------------------------------------------------------------------
// The op runner (host_vision_jobs.cpp)
// ---------------------------------------------------------------------------

// `compute` runs on the worker thread and touches no Value; it may throw and
// the message reaches `done` as `error`. `build` and `release` run on the JS
// thread. `release` clears the model's busy flag and drops the keep-alive
// root, and runs BEFORE the callback so an onDone that starts the next op on
// the same model finds it free — the chained-step case the old binding named.
using VisionComputeFn = std::function<void(const std::atomic<bool>& cancel)>;
using VisionBuildFn = std::function<Value()>;
using VisionReleaseFn = std::function<void()>;

// Synchronous when `onDone` is not a function: compute inline, return
// build()'s value (or throw). Asynchronous when it is: return an AsyncHandle
// with `.cancel()` and later call onDone(result, {cancelled, error?}).
// `selfRef` is the model value, rooted for the life of an async job so a
// collection cannot free the C++ the worker is reading.
Value runVisionOp(Value onDone, Value selfRef, VisionComputeFn compute,
                  VisionBuildFn build, VisionReleaseFn release);

// `opts.onDone`, or undefined for a non-object / absent option bag.
Value visionOnDone(Value opts);

// One op at a time per model: the wrappers hold no lock while the worker
// runs, so a second call on the same model would hand two threads the same
// brotensor workspace. Keyed by the wrapper pointer, per thread (a model
// belongs to the realm that made it).
bool visionMarkBusy(const void* model);
void visionClearBusy(const void* model);

// ---------------------------------------------------------------------------
// Rasterizers (host_vision_jobs.cpp)
// ---------------------------------------------------------------------------
// Each returns an ImageBitmap Value (null if the size is empty). These are
// the old binding's makeBitmap family, unchanged in what they produce.

// RGBA8 straight through.
Value visionBitmapRGBA(const uint8_t* rgba, int w, int h);
// Interleaved HxWx3 RGB, alpha forced opaque.
Value visionBitmapRGB(const uint8_t* rgb, int w, int h);
// A scalar map min/max normalized to gray; `invert` flips it. Writes the
// observed range to lo/hi, which is what the depth result reports as
// min/max.
Value visionBitmapGrayNormalized(const std::vector<float>& map, int w, int h,
                                 bool invert, float& lo, float& hi);
// A scalar map already in [0,1] (edge / line probability / matte), no
// rescale.
Value visionBitmapGrayUnit(const std::vector<float>& map, int w, int h,
                           bool invert);
// A 0/1 mask as a colored overlay: foreground (r,g,b,255), background fully
// transparent.
Value visionBitmapMask(const uint8_t* mask, int w, int h, uint8_t r, uint8_t g,
                       uint8_t b);
// Planar NCHW (3,H,W) unit normals through (n+1)/2.
Value visionBitmapNormals(const std::vector<float>& nchw, int w, int h);
// White 1px lines on transparent black, for the MLSD segment overlay.
Value visionBitmapSegments(const std::vector<float>& x1, const std::vector<float>& y1,
                           const std::vector<float>& x2, const std::vector<float>& y2,
                           int w, int h);

// ---------------------------------------------------------------------------
// The wrapped families (host_vision_ops.cpp, host_vision_annotators.cpp)
// ---------------------------------------------------------------------------

void installVisionDepthOps();       // DepthEstimator, NormalEstimator
void installVisionSamOps();         // Sam.segment / segmentEverything
void installVisionGenerativeOps();  // BiRefNet, StyleGAN3
void installVisionAnnotatorOps();   // Hed, Lineart, Mlsd, Openpose, Segformer

}  // namespace bro::bronze_host

#endif  // BRO_WITH_VISION
