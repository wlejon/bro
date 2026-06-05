#pragma once

// JS bindings for TripoSplat — single-image -> 3D Gaussian Splat (bro.triposplat).
//
// Composition layer: TripoSplat is not one library but a pipeline assembled from
// siblings — DINOv3 ViT-H (brovisionml) + Flux.2 VAE encoder, flow-matching DiT
// and octree Gaussian decoder (brodiffusion). Each piece owns its own
// correctness (golden-tested in its repo); this binding is purely the
// cross-library glue the bro runtime is the natural home for: preprocess the
// image, run the two encoders, draw seeded noise, run the Euler CFG sampler,
// decode to a Gaussian cloud, and hand the splats to JS (which feeds the scene
// GaussianSplatNode renderer).

struct JSContext;

namespace bro::js {

// Install bro.triposplat.{init, load} and the TripoSplatPipeline class.
void installTriposplatBindings(JSContext* ctx);

}  // namespace bro::js
