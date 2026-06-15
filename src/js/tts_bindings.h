#pragma once

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

// Install the `bro.tts` namespace — text-to-speech via the brosoundml sibling.
// Exposes three pipelines as opaque handle classes:
//   Kokoro (Kokoro, Voice) via bro.tts.loadKokoro(modelDir) — the 82M
//     phoneme-driven pipeline.
//   QwenTts via bro.tts.loadQwen(modelDir) — Qwen3-TTS, the 12 Hz
//     multi-codebook model. Text-driven end-to-end (no phoneme frontend, no
//     voice pack); preset CustomVoice speakers via opts.speaker.
//   Supertonic (Supertonic, SupertonicVoice) via bro.tts.loadSupertonic(modelDir)
//     — Supertonic-3, a ~99M flow-matching multilingual model. Text-driven with a
//     codepoint frontend (no G2P); a voice is a VoiceStyle preset loaded from the
//     model's voice_styles/ via model.loadVoiceStyle(path). Output is 44.1 kHz.
//   SpeakerEncoder via bro.tts.loadSpeakerEncoder(dir) — the standalone
//     ECAPA-TDNN x-vector extractor (~18 MB artifact). embedSpeaker(audio)
//     enrolls a voice-clone reference without loading all of Qwen-Base.
//
// Kokoro takes already-tokenized phoneme ids. A G2P frontend is wired in via
// bro.tts.phonemize(text) (brosoundml's in-tree English Phonemizer) and via
// the Kokoro handle's encodePhonemes(ipa) (just the IPA-codepoint adapter).
// Output of synthesize() is a mono 24 kHz Float32Array, drop-in for an
// AudioBuffer or a WAV write.
//
// bro.tts.synthesize(model, ...) dispatches on the model type: a Kokoro takes
// (kokoro, phonemeIds, voice, opts?); a QwenTts takes (qwen, text, opts?); a
// Supertonic takes (supertonic, text, opts?) with opts.voice a SupertonicVoice.
// All run on a background thread and are cancellable via the returned handle.
//
// Runs on GPU by default — loadKokoro / loadQwen place the model on CUDA when a
// GPU backend is available (opts.device: 'cuda' | 'cpu' to override).
void installTtsBindings(JSContext* ctx);

// Symmetric cleanup hook. No-op today.
void cleanupTtsBindings(JSContext* ctx);

}  // namespace bro::js
