// End-to-end smoke test for bro.stt + bro.lm + bro.tts.
//
// Loads each model from its sibling repo's weights/ dir, runs one
// transcription / generation / synthesis pass, and prints a one-line
// summary per stage. The output WAV from Kokoro is written next to this
// script so we can listen to it.
//
// Run from bro repo root:
//   ./build/Debug/bro-headless.exe ../broworkshop tests/smoke_voice_pipeline.js

const FS = require('node:fs');

// ─── tiny 16-bit PCM WAV reader ────────────────────────────────────────────
function readWav16(path) {
    const buf = FS.readFileSync(path);
    const ab  = buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength);
    const dv  = new DataView(ab);
    if (dv.getUint32(0, false) !== 0x52494646)
        throw new Error(`${path}: not a RIFF file`);
    const sampleRate   = dv.getUint32(24, true);
    const numChannels  = dv.getUint16(22, true);
    const bitsPerSample = dv.getUint16(34, true);
    if (bitsPerSample !== 16)
        throw new Error(`${path}: expected 16-bit PCM, got ${bitsPerSample}-bit`);

    // Scan for the 'data' chunk (some WAVs carry LIST/INFO chunks first).
    let offset = 12;
    while (offset < ab.byteLength) {
        const id   = String.fromCharCode(dv.getUint8(offset),   dv.getUint8(offset+1),
                                          dv.getUint8(offset+2), dv.getUint8(offset+3));
        const size = dv.getUint32(offset + 4, true);
        if (id === 'data') {
            const pcm = new Int16Array(ab, offset + 8, size / 2);
            // Downmix to mono if needed.
            const frameCount = pcm.length / numChannels;
            const samples = new Float32Array(frameCount);
            for (let i = 0; i < frameCount; i++) {
                let s = 0;
                for (let c = 0; c < numChannels; c++) s += pcm[i * numChannels + c];
                samples[i] = (s / numChannels) / 32768;
            }
            return { samples, sampleRate };
        }
        offset += 8 + size;
    }
    throw new Error(`${path}: no 'data' chunk`);
}

// ─── tiny 16-bit PCM mono WAV writer ───────────────────────────────────────
function writeWav16(path, samples, sampleRate) {
    const n   = samples.length;
    const buf = new ArrayBuffer(44 + n * 2);
    const dv  = new DataView(buf);
    // RIFF header
    dv.setUint32(0, 0x52494646, false); // 'RIFF'
    dv.setUint32(4, 36 + n * 2, true);
    dv.setUint32(8, 0x57415645, false); // 'WAVE'
    // fmt chunk
    dv.setUint32(12, 0x666d7420, false); // 'fmt '
    dv.setUint32(16, 16, true);
    dv.setUint16(20, 1,  true);          // PCM
    dv.setUint16(22, 1,  true);          // mono
    dv.setUint32(24, sampleRate, true);
    dv.setUint32(28, sampleRate * 2, true);
    dv.setUint16(32, 2, true);
    dv.setUint16(34, 16, true);
    // data chunk
    dv.setUint32(36, 0x64617461, false); // 'data'
    dv.setUint32(40, n * 2, true);
    for (let i = 0; i < n; i++) {
        let s = samples[i];
        if (s >  1) s =  1;
        if (s < -1) s = -1;
        dv.setInt16(44 + i * 2, Math.round(s * 32767), true);
    }
    FS.writeFileSync(path, new Uint8Array(buf));
}

function ms(t0) { return ((Date.now() - t0) / 1000).toFixed(2) + 's'; }

// ════════════════════════════════════════════════════════════════════════════
// Stage 1 — Whisper STT
// ════════════════════════════════════════════════════════════════════════════
console.log('\n── Stage 1: bro.stt (Whisper) ─────────────────────────');
{
    const wavPath = '../brosoundml/weights/whisper/test_audio_en.wav';
    const audio = readWav16(wavPath);
    console.log(`audio: ${audio.samples.length} samples @ ${audio.sampleRate} Hz`);

    const t0 = Date.now();
    const whisper = bro.stt.loadWhisper('../brosoundml/weights/whisper');
    console.log(`loadWhisper: ${ms(t0)}  d_model=${whisper.dModel} layers/enc/dec`);

    const tok = bro.stt.loadTokenizer({
        vocabPath:  '../brosoundml/weights/whisper/vocab.json',
        mergesPath: '../brosoundml/weights/whisper/merges.txt',
    });

    const prompt = tok.buildPrompt('en', 'transcribe', false);
    const t1 = Date.now();
    const ids = whisper.transcribe(audio, prompt, { maxNewTokens: 96 });
    console.log(`transcribe: ${ms(t1)}  ${ids.length} tokens`);
    const text = tok.decode(ids, /*skipSpecial=*/true);
    console.log(`STT: "${text.trim()}"`);
}

// ════════════════════════════════════════════════════════════════════════════
// Stage 2 — Qwen3 LLM
// ════════════════════════════════════════════════════════════════════════════
console.log('\n── Stage 2: bro.lm (Qwen3) ────────────────────────────');
{
    const t0 = Date.now();
    const { model, tokenizer } =
        bro.lm.loadQwen('../brolm/weights/Qwen3-0.6B-GGUF/Qwen3-0.6B-BF16.gguf');
    console.log(`loadQwen: ${ms(t0)}  vocab=${model.vocabSize}  hidden=${model.hiddenSize}  layers=${model.numLayers}`);

    const prompt = tokenizer.applyChatTemplate([
        { role: 'system', content: 'You are concise. Reply in one short sentence.' },
        { role: 'user',   content: 'Say hello to a new friend named Bro.' },
    ], /*addGenerationPrompt=*/true);
    const promptIds = tokenizer.encode(prompt);
    console.log(`prompt: ${promptIds.length} tokens`);

    model.allocateCache(promptIds.length + 64);
    const t1 = Date.now();
    const newIds = model.generate(promptIds, {
        maxNewTokens: 40,
        eosId:        tokenizer.imEndId,
        sampling:     { temperature: 0.7, topK: 40, topP: 0.95, seed: 42 },
    });
    console.log(`generate: ${ms(t1)}  ${newIds.length} new tokens`);
    const reply = tokenizer.decode(newIds);
    console.log(`LLM: "${reply.trim()}"`);
}

// ════════════════════════════════════════════════════════════════════════════
// Stage 3 — Kokoro TTS
// ════════════════════════════════════════════════════════════════════════════
console.log('\n── Stage 3: bro.tts (Kokoro) ──────────────────────────');
{
    const t0 = Date.now();
    const kokoro = bro.tts.loadKokoro('../brosoundml/weights/kokoro');
    console.log(`loadKokoro: ${ms(t0)}  nTokens=${kokoro.nTokens}  styleDim=${kokoro.styleDim}`);

    const voice = kokoro.loadVoice('../brosoundml/weights/kokoro/voices/af_heart.bin');
    console.log(`voice: ${voice.name || 'af_heart'}  ${voice.rows}×${voice.cols}`);

    // Reference phoneme id sequence — first line of ids.txt is a known-good
    // sample so we hit the end-to-end synth path without bundling a G2P.
    const refIds = FS.readFileSync('../brosoundml/weights/kokoro/ids.txt', 'utf8')
                     .split('\n')[0]
                     .split(',')
                     .map(s => parseInt(s, 10));
    console.log(`phonemes: ${refIds.length} ids`);

    const t1 = Date.now();
    const out = kokoro.synthesize(refIds, voice, { speed: 1.0 });
    console.log(`synthesize: ${ms(t1)}  ${out.samples.length} samples @ ${out.sampleRate} Hz`);

    const outPath = 'tests/smoke_voice_out.wav';
    writeWav16(outPath, out.samples, out.sampleRate);
    console.log(`TTS: wrote ${outPath} (${(out.samples.length / out.sampleRate).toFixed(2)}s)`);
}

console.log('\nAll three stages succeeded.');
