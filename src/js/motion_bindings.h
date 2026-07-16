#pragma once

// JS bindings for ARDY text-to-motion (bro.motion).
//
// Composition layer: ARDY (nvidia/ARDY-G1-RP) is a text-conditioned
// autoregressive diffusion motion model whose C++ port spans two siblings —
// the LLM2Vec text encoder + Llama-3 tokenizer (brolm) and the motion denoiser,
// FSQ decoder, autoregressive rollout and G1 forward-kinematics motion rep
// (brodiffusion::ardy). Each piece owns its own correctness (golden-tested in
// its repo); this binding is the cross-library glue: encode the prompt into the
// pooled conditioning feature, roll out the hybrid motion, detokenize to
// explicit features, run FK, and hand per-frame G1 joint positions to JS (which
// drives a scene skeleton / mesh).
//
//   const m = bro.motion.load({ checkpoint, textEncoder, device });
//   const clip = m.generate("a person walks forward and waves",
//                           { frames: 104, steps: 10, cfg: 2.5, seed: 0 });
//   // clip = { frames, joints, fps, positions, parents, footContacts }
//
// Heavy (loads an 8B text encoder + the motion model, multi-second generate) —
// run inside a Worker for a responsive UI; the binding is installed there too.

struct JSContext;

namespace bro::js {

// Install bro.motion.{init, load} and the ArdyMotionPipeline class.
void installMotionBindings(JSContext* ctx);

}  // namespace bro::js
