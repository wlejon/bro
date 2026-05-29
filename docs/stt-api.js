/**
 * bro.stt — Speech-to-text (Whisper)
 *
 * Transcribes 16 kHz mono audio to text with a Whisper encoder/decoder.
 * Backed by brosoundml (audio-ML inference) on top of brotensor. Defaults to
 * CUDA; pass { device: 'cpu' } to force the CPU backend.
 *
 * A transcription needs two pieces: the Whisper model (loadWhisper) and a
 * BPE tokenizer (loadTokenizer) for building the decoder prompt and decoding
 * the output ids back to text.
 *
 * Audio is supplied as { samples: Float32Array, sampleRate: number } in the
 * [-1, 1] range. Whisper expects 16 kHz mono — resample/downmix before
 * calling (see tests/smoke_voice_pipeline.js for a WAV reader that does this).
 */


// ── Load ──────────────────────────────────────────────────────────────────

/**
 * Load a Whisper model from a weights directory (model.safetensors + config).
 *
 * @param {string} dir            - Whisper weights directory.
 * @param {Object} [opts]
 * @param {string} [opts.device='cuda'] - 'cuda' or 'cpu'.
 * @returns {WhisperModel}  - has .dModel and .transcribe()
 */
const whisper = bro.stt.loadWhisper('../brosoundml/weights/whisper');
// whisper.dModel === 384 (tiny)

/**
 * Load the Whisper BPE tokenizer.
 *
 * @param {Object} opts
 * @param {string} opts.vocabPath  - path to vocab.json
 * @param {string} opts.mergesPath - path to merges.txt
 * @returns {WhisperTokenizer}
 */
const tok = bro.stt.loadTokenizer({
    vocabPath:  '../brosoundml/weights/whisper/vocab.json',
    mergesPath: '../brosoundml/weights/whisper/merges.txt',
});


// ── Tokenizer prompt + decode ───────────────────────────────────────────────

/**
 * WhisperTokenizer
 *
 * @method buildPrompt(lang, task, timestamps) → number[]
 *         Build the decoder start prompt. lang is a language code ('en'),
 *         task is 'transcribe' or 'translate', timestamps is a boolean that
 *         toggles the <|notimestamps|> token.
 *
 * @method decode(ids, skipSpecial) → string
 *         Decode output token ids to text. Pass skipSpecial=true to strip
 *         <|...|> control tokens.
 */
const prompt = tok.buildPrompt('en', 'transcribe', /*timestamps=*/false);


// ── Transcribe ──────────────────────────────────────────────────────────────

/**
 * WhisperModel
 *
 * @property {number} dModel
 *
 * @method transcribe(audio, prompt, opts) → number[]
 *         Run the encoder over `audio` and greedily decode starting from
 *         `prompt`, returning the generated token ids.
 *
 *         audio: { samples: Float32Array, sampleRate: number }  (16 kHz mono)
 *         opts:  { maxNewTokens } — cap on decoded tokens.
 */
const audio = { samples: /* Float32Array */ null, sampleRate: 16000 };
const ids   = whisper.transcribe(audio, prompt, { maxNewTokens: 96 });
const text  = tok.decode(ids, /*skipSpecial=*/true);
console.log(text.trim());
