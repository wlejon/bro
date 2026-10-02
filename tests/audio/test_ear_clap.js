// Weights-gated test for bro.ear.loadClap (docs/ear-api.js): the CLAP scorer
// brosoundml mounts onto the bro.ear object broaudio installs, driven the way
// a generate-score-keep loop drives it.
//
// 1. Both halves share one bro.ear: measure / compare / spectrogram beside
//    loadClap.
// 2. score() answers one 0..1 score per prompt summing to 1, raw cosines,
//    logits = cosines * logitScale, and the clip's unit-length embedding.
// 3. Deterministic: the same clip scores to the same bits twice, embedAudio
//    matches score's embedding, and cached text embeddings from embedText
//    score exactly as the prompt strings do (best is then null).
// 4. The anti-prompt reading works on clips with a known character: a ringing
//    modal bar leans to "xylophone" and a burst of noise to "static noise".
//
// Skips without the weights (BRO_WEIGHTS or ../brosoundml/weights) or a GPU.

const fs = require('node:fs');

const WEIGHTS = process.env.BRO_WEIGHTS || '../brosoundml/weights';
const CLAP_DIR = WEIGHTS + '/clap';

const sr = 48000;

function bar(f0, secs) {
    const s = new Float32Array(Math.floor(secs * sr));
    const modes = [[1, 0.5, 0.8], [2.756, 0.3, 0.5], [5.404, 0.2, 0.3]];
    for (let i = 0; i < s.length; i++) {
        const t = i / sr;
        let v = 0;
        for (const [r, a, tau] of modes) v += a * Math.exp(-t / tau) * Math.sin(2 * Math.PI * f0 * r * t);
        s[i] = v;
    }
    return s;
}

function noise(secs) {
    const s = new Float32Array(Math.floor(secs * sr));
    let seed = 7;
    for (let i = 0; i < s.length; i++) {
        seed = (seed * 1103515245 + 12345) % 2147483648;
        s[i] = 0.5 * (seed / 1073741824 - 1);
    }
    return s;
}

function sameBits(a, b) {
    if (a.length !== b.length) return false;
    const ua = new Uint32Array(a.buffer, a.byteOffset, a.length);
    const ub = new Uint32Array(b.buffer, b.byteOffset, b.length);
    for (let i = 0; i < ua.length; i++) if (ua[i] !== ub[i]) return false;
    return true;
}

assert(bro.ear && typeof bro.ear.measure === 'function', 'bro.ear.measure is installed');

if (typeof bro.ear.loadClap !== 'function') {
    console.log('SKIP: bro.ear.loadClap is absent (built without BRO_WITH_SOUNDML)');
} else if (!fs.existsSync(CLAP_DIR + '/model.safetensors')) {
    console.log('SKIP: CLAP weights not found at ' + CLAP_DIR);
} else if (!bro.gpu.available) {
    console.log('SKIP: no GPU backend (' + bro.gpu.backend + ')');
} else {
    assert(typeof bro.ear.compare === 'function' && typeof bro.ear.spectrogram === 'function',
           'loadClap sits beside compare / spectrogram on one bro.ear');

    const clap = bro.ear.loadClap(CLAP_DIR);
    assert(clap instanceof bro.ear.ClapModel, 'loadClap returns a ClapModel');
    assert(clap.loaded && clap.device.toLowerCase() === bro.gpu.backend,
           'loaded on the default GPU (' + bro.gpu.backend + '), got ' + clap.device);
    assert(clap.sampleRate === 48000 && clap.embeddingSize === 512, 'CLAP shape');

    const prompts = ['a xylophone', 'static noise', 'a dog barking'];
    const barClip = { samples: bar(523.25, 2.0), sampleRate: sr };
    const noiseClip = { samples: noise(2.0), sampleRate: sr };

    const rb = clap.score(barClip, prompts);
    const rn = clap.score(noiseClip, prompts);
    console.log('bar:', Array.from(rb.scores).map(v => v.toFixed(3)).join(' '),
                'noise:', Array.from(rn.scores).map(v => v.toFixed(3)).join(' '));

    // 2. result shape
    assert(rb.scores.length === 3 && rb.similarities.length === 3 && rb.logits.length === 3,
           'one score / similarity / logit per prompt');
    let sum = 0, norm = 0;
    for (const v of rb.scores) { assert(v >= 0 && v <= 1, 'score in 0..1'); sum += v; }
    assert(Math.abs(sum - 1) < 1e-5, 'scores sum to 1, got ' + sum);
    for (let i = 0; i < 3; i++)
        assert(Math.abs(rb.logits[i] - rb.similarities[i] * clap.logitScale) < 1e-3,
               'logit = cosine * logitScale');
    for (const v of rb.embedding) norm += v * v;
    assert(rb.embedding.length === 512 && Math.abs(norm - 1) < 1e-4, 'unit-length 512-d embedding');
    assert(rb.best === prompts[rb.bestIndex], 'best names the winning prompt');

    // 3. determinism and caching
    assert(sameBits(clap.score(barClip, prompts).scores, rb.scores), 'same clip, same bits');
    assert(sameBits(clap.embedAudio(barClip), rb.embedding), 'embedAudio matches score().embedding');
    const text = clap.embedText(prompts);
    assert(Array.isArray(text) && text.length === 3 && text[0].length === 512, 'embedText per prompt');
    const rc = clap.score(barClip, text);
    assert(sameBits(rc.scores, rb.scores), 'cached text embeddings score as the strings do');
    assert(rc.best === null && rc.bestIndex === rb.bestIndex, 'best is null for a cached embedding');
    assert(sameBits(clap.scoreEmbedding(rb.embedding, text).scores, rb.scores),
           'scoreEmbedding rescores without the audio tower');

    // 4. anti-prompt reading on known clips
    assert(rb.similarities[0] > rb.similarities[1], 'the bar is closer to xylophone than to noise');
    assert(rn.similarities[1] > rn.similarities[0], 'the noise is closer to static than to xylophone');

    clap.dispose();
    console.log('test_ear_clap: OK');
}
