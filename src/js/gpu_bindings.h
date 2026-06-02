#pragma once

#include <quickjs.h>

namespace bro::js {

// Installs the always-present `bro.gpu` namespace — a runtime probe of the
// brotensor backends registered in this binary. Unlike `bro.tensor` (which
// compiles out to a stub without a GPU backend), `bro.gpu` exists in every
// build because brotensor's CPU backend is always linked; it reports what the
// ML model loaders (bro.lm / bro.stt / bro.tts / bro.vision / bro.diffusion)
// will actually default to, so apps can warn before loading a large model on
// CPU. Exposes lazy getters (the CUDA/Metal driver probe runs on first access,
// not at context creation):
//   bro.gpu.available  — bool: a GPU device is registered and is the default
//   bro.gpu.backend    — string: 'cuda' | 'metal' | 'cpu' (the default device)
//   bro.gpu.devices    — string[]: every registered device (e.g. ['cpu','cuda'])
void installGpuBindings(JSContext* ctx);

} // namespace bro::js
