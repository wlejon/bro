#pragma once

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

// Install the `bro.vision` namespace — vision-model inference via the
// brovisionml sibling (SAM segmentation, Depth-Anything depth, DSINE surface
// normals, and the ControlNet conditioning annotators: HED soft-edge, lineart,
// MLSD straight lines, OpenPose body pose, SegFormer semantic segmentation).
//
// Each model is an opaque handle class (Sam, DepthEstimator, …) created by a
// `bro.vision.loadXxx(modelDir, opts)` loader. Models run on GPU by default —
// loaders place the model on CUDA when a GPU backend is available; pass
// opts.device 'cpu' to force CPU. Heavy work (load, image encode, inference)
// runs on a background thread when an onReady/onDone callback is supplied, so
// the JS thread stays responsive (the same async-job convention bro.stt /
// bro.tts / bro.lm use); a synchronous fallback runs inline when no callback
// is given.
//
// Image inputs accept an `ImageBitmap` or an ImageData-shaped
// `{ data, width, height }` (RGBA Uint8). Dense-map results come back as a
// drawable `ImageBitmap` (colorized / grayscale) plus the raw typed-array data.
void installVisionBindings(JSContext* ctx);

// Symmetric cleanup hook. No-op today.
void cleanupVisionBindings(JSContext* ctx);

}  // namespace bro::js
