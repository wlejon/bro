// Weights-gated test for the CLAP short-clip padding option (docs/ear-api.js,
// ClapClipOptions.pad): how a clip under CLAP's 10 s window is filled out.
//
// 1. pad: 'silence' embeds exactly what the clip followed by zeros to 10 s
//    embeds, and pad: 'repeat' exactly what the clip tiled as many whole
//    times as fit, then zeros (transformers' "repeatpad"), embeds.
// 2. The default ('auto') is 'silence' for a 0.3 s one-shot, including one
//    given at 44.1 kHz, and 'repeat' for a 3 s clip.
// 3. A clip over 10 s is never padded, so pad changes nothing for it.
// 4. A pad that is not 'silence' / 'repeat' / 'auto' throws a TypeError.
// The gunshot-vs-drum-loop scores under each pad are logged, not asserted.
//
// Skips without the weights (BRO_WEIGHTS or ../brosoundml/weights) or a GPU.

const fs = require('node:fs');

const WEIGHTS = process.env.BRO_WEIGHTS || '../brosoundml/weights';
const CLAP_DIR = WEIGHTS + '/clap';
const sr = 48000;
const WINDOW = 10 * sr;

// A decaying noise burst: a gunshot-like one-shot.
function shot(secs, rate) {
    const s = new Float32Array(Math.floor(secs * rate));
    let seed = 11;
    for (let i = 0; i < s.length; i++) {
        seed = (seed * 1103515245 + 12345) % 2147483648;
        s[i] = 0.8 * Math.exp(-i / (0.04 * rate)) * (seed / 1073741824 - 1);
    }
    return s;
}

function tone(secs) {
    const s = new Float32Array(Math.floor(secs * sr));
    for (let i = 0; i < s.length; i++) s[i] = 0.3 * Math.sin(2 * Math.PI * 440 * i / sr);
    return s;
}

function silencePadded(clip) {
    const w = new Float32Array(WINDOW);
    w.set(clip, 0);
    return w;
}

function repeatPadded(clip) {
    const w = new Float32Array(WINDOW);
    const reps = Math.floor(WINDOW / clip.length);
    for (let r = 0; r < reps; r++) w.set(clip, r * clip.length);
    return w;
}

function sameBits(a, b) {
    if (a.length !== b.length) return false;
    const ua = new Uint32Array(a.buffer, a.byteOffset, a.length);
    const ub = new Uint32Array(b.buffer, b.byteOffset, b.length);
    for (let i = 0; i < ua.length; i++) if (ua[i] !== ub[i]) return false;
    return true;
}

if (typeof bro.ear.loadClap !== 'function') {
    console.log('SKIP: bro.ear.loadClap is absent (built without BRO_WITH_SOUNDML)');
} else if (!fs.existsSync(CLAP_DIR + '/model.safetensors')) {
    console.log('SKIP: CLAP weights not found at ' + CLAP_DIR);
} else if (!bro.gpu.available) {
    console.log('SKIP: no GPU backend (' + bro.gpu.backend + ')');
} else {
    const clap = bro.ear.loadClap(CLAP_DIR);
    assert(clap.windowSeconds === 10 && clap.sampleRate === sr, 'CLAP window is 10 s at 48 kHz');

    // 1. Each pad is exactly its manual 10 s window.
    const g = shot(0.3, sr);
    const eSilence = clap.embedAudio(g, { pad: 'silence' });
    const eRepeat = clap.embedAudio(g, { pad: 'repeat' });
    assert(sameBits(eSilence, clap.embedAudio(silencePadded(g))), "pad 'silence' = the clip then zeros to 10 s");
    assert(sameBits(eRepeat, clap.embedAudio(repeatPadded(g))), "pad 'repeat' = whole tiles then zeros");
    assert(!sameBits(eSilence, eRepeat), 'silence and repeat padding embed differently');

    // 2. The default: silence for a one-shot, repeat for a 3 s clip.
    assert(sameBits(clap.embedAudio(g), eSilence), 'a 0.3 s clip is silence-padded by default');
    assert(sameBits(clap.embedAudio(g, { pad: 'auto' }), eSilence), "pad 'auto' is the default");
    const g441 = { samples: shot(0.3, 44100), sampleRate: 44100 };
    assert(sameBits(clap.embedAudio(g441), clap.embedAudio(g441, { pad: 'silence' })),
           'a 0.3 s 44.1 kHz clip is silence-padded by default');
    const t3 = tone(3);
    const t3Repeat = clap.embedAudio(t3, { pad: 'repeat' });
    assert(sameBits(clap.embedAudio(t3), t3Repeat), 'a 3 s clip is repeat-padded by default');
    assert(!sameBits(clap.embedAudio(t3, { pad: 'silence' }), t3Repeat), 'a 3 s clip can be silence-padded');

    // The default reaches score() too.
    const prompts = ['a single gunshot', 'a drum loop', 'a fast rhythmic drum pattern'];
    const rDefault = clap.score(g, prompts);
    assert(sameBits(rDefault.embedding, eSilence), 'score() pads as embedAudio does');
    const rRepeat = clap.score(g, prompts, { pad: 'repeat' });
    const fmt = (r) => Array.from(r.scores, (v, i) => prompts[i] + ' ' + v.toFixed(3)).join(', ');
    console.log('  0.3 s shot, silence: ' + fmt(rDefault));
    console.log('  0.3 s shot, repeat:  ' + fmt(rRepeat));

    // 3. Over 10 s: nothing to pad.
    const long = tone(10.5);
    assert(sameBits(clap.embedAudio(long, { pad: 'silence' }), clap.embedAudio(long, { pad: 'repeat' })),
           'pad does not touch a clip over 10 s');

    // 4. A bad pad is a TypeError, not a silent default.
    let threw = null;
    try { clap.embedAudio(g, { pad: 'zeros' }); } catch (e) { threw = e; }
    assert(threw instanceof TypeError && /opts\.pad/.test(String(threw.message)),
           "pad 'zeros' throws a TypeError naming opts.pad, got " + threw);

    clap.dispose();
    console.log('test_ear_clap_pad: OK');
}
