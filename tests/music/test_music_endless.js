// yue.endless (YuE2Endless) and bro.music.beat through JS (docs/music-endless-api.js).
// The stream's own oracles live in brosoundml (test_yue2_endless: unpaced
// exactness, drops, paced branches and commits, service gaps) and in
// yue2-research (native/endless_stream.js: endless.js bit for bit); this
// checks the binding.
//
//   1. bro.music.beat on a click track (90 bpm, first click at 0.25 s): the
//      energy sensor's phase within 0.03 beats, the spectral one's ~0.04 s
//      early (as documented); a stereo planar buffer reads as its mono;
//      bro.ear.beat is the same; argument errors are TypeErrors.
//   Weights-gated (the YuE2-3B folder, $YUE2_MODEL_DIR else ../YuE2-3B, and
//   the YuE2 VAE, BRO_WEIGHTS or ../brosoundml/weights /yue2-vae, on a GPU):
//   2. Argument errors are TypeErrors; a refused stream claims nothing.
//   3. Unpaced, 2 chunks with chunkData: 'sampled' / 'built' per chunk in
//      order with 250 tokens and 250 x 64 latents, rows with their tag and
//      memory; contiguous planar audio from 0 that is exactly vae.decode()
//      of the latents; 'done', onDone, the gates busy then free.
//   4. Paced by a simulated player: stats has its shape; setTarget() is
//      planned at stats.openChunk and returns the merged target; setKnobs()
//      clamps; branch() returns 1 and a 'branch' event follows, discardBranch
//      a 'discarded'; stop() ends it cleanly.

const fs = require('node:fs');

function expectThrows(fn, what) {
    let err = null;
    try { fn(); } catch (e) { err = e; }
    assert(err !== null, what + ' throws');
    return err;
}

function pumpUntil(desc, fn, seconds) {
    const iters = Math.ceil((seconds * 1000) / 16);
    for (let i = 0; i < iters; i++) {
        advanceTime(16);
        wallSleep(16);
        if (fn()) return;
    }
    throw new Error('timeout waiting for ' + desc);
}

const SR = 48000;
const FOLK = 'folk, acoustic guitar, warm, intimate, male vocal, 90 bpm';
const SYNTH = 'synthwave, analog synthesizer, drum machine, retro, 100 bpm';
const MODEL_DIR = process.env.YUE2_MODEL_DIR || '../YuE2-3B';
const VAE_DIR = (process.env.BRO_WEIGHTS || '../brosoundml/weights') + '/yue2-vae';

if (!bro.music || bro.music.available === false || typeof bro.music.beat !== 'function') {
    skipTest('bro.music is not in this build');
} else {
    beat();
    if (!fs.existsSync(MODEL_DIR + '/model.safetensors') || !fs.existsSync(VAE_DIR + '/model.safetensors')) {
        skipTest('beat passed; the stream needs ' + MODEL_DIR + ' and ' + VAE_DIR);
    } else if (!bro.gpu || !bro.gpu.available) {
        skipTest('beat passed; the stream needs a GPU');
    } else {
        const yue = bro.music.loadYuE2(MODEL_DIR);
        const vae = bro.music.loadVae(VAE_DIR, { decoderOnly: true });
        argumentErrors(yue, vae);
        unpaced(yue, vae);
        paced(yue, vae);
    }
}

function clickTrack(seconds, period, first) {
    const n = Math.floor(seconds * SR), x = new Float32Array(n);
    for (let i = 0; i < n; i++) x[i] = 0.03 * Math.sin(2 * Math.PI * 220 * i / SR);
    let s = 12345;
    for (let t = first; t < seconds; t += period) {
        const i0 = Math.floor(t * SR);
        for (let j = 0; j < 0.012 * SR && i0 + j < n; j++) {
            s = (Math.imul(s, 1664525) + 1013904223) >>> 0;
            x[i0 + j] += 0.8 * ((s / 4294967296) * 2 - 1) * Math.exp(-j / (0.003 * SR));
        }
    }
    return x;
}

function phaseDistance(a, b) {
    const d = Math.abs(a - b) % 1;
    return Math.min(d, 1 - d);
}

function beat() {
    const P = 2 / 3, first = 0.25, x = clickTrack(10, P, first);
    const t0 = Date.now();
    const sp = bro.music.beat(x, { bpm: 90 });
    const ms = Date.now() - t0;
    const en = bro.music.beat(x, { period: P, sensor: 'energy', envelope: true });
    console.log('beat: spectral phase ' + sp.phase.toFixed(4) + ' contrast ' + sp.contrast.toFixed(1) + ' (' + ms +
                ' ms for 10 s); energy phase ' + en.phase.toFixed(4) + ' contrast ' + en.contrast.toFixed(1));
    assert(Math.abs(sp.period - P) < 1e-9 && Math.abs(sp.periodFrames - 100 * P) < 1e-6, 'period in seconds and frames');
    assert(phaseDistance(en.phase, first / P) < 0.03, 'energy phase ' + en.phase + ' ~ ' + first / P);
    assert(phaseDistance(sp.phase, (first - 0.04) / P) < 0.03, 'spectral phase ' + sp.phase + ' ~0.04 s early');
    assert(Math.abs(en.firstBeat - en.phase * en.period) < 1e-9, 'firstBeat = phase * period');
    assert(sp.contrast > 2 && en.contrast > 2 && sp.beats >= 14, 'a click track has a clear beat');
    assert(en.envelope instanceof Float32Array && en.envelope.length === 1000, 'envelope: 100 frames a second');
    assert(sp.envelope === undefined, 'no envelope unless asked');
    // Planar stereo reads as its mono; bro.ear.beat is the same sensor.
    const st = new Float32Array(2 * x.length);
    st.set(x, 0);
    st.set(x, x.length);
    const s2 = bro.music.beat({ samples: st, channels: 2, sampleRate: SR }, { bpm: 90 });
    assert(s2.phase === sp.phase && s2.strength === sp.strength, 'planar stereo of x reads as x');
    if (bro.ear && typeof bro.ear.beat === 'function') {
        const e2 = bro.ear.beat(x, { bpm: 90 });
        assert(e2.phase === sp.phase && e2.contrast === sp.contrast, 'bro.ear.beat is bro.music.beat');
    }
    const searched = bro.music.beat(x, { period: P * 1.02, searchPeriod: true });
    assert(Math.abs(searched.period / P - 1) < 0.005, 'searchPeriod finds the period: ' + searched.period);
    let err = expectThrows(() => bro.music.beat(x, {}), 'beat without a period');
    assert(err instanceof TypeError && /period/.test(err.message), 'a missing period is named: ' + err.message);
    err = expectThrows(() => bro.music.beat(x, { bpm: 90, sensor: 'latent' }), 'a bad sensor');
    assert(err instanceof TypeError && /sensor/.test(err.message), 'a bad sensor is named: ' + err.message);
    err = expectThrows(() => bro.music.beat({ samples: x, channels: 1, sampleRate: 44100 }, { bpm: 90 }), '44.1 kHz');
    assert(err instanceof TypeError && /48 kHz/.test(err.message), 'the rate is named: ' + err.message);
}

function argumentErrors(yue, vae) {
    let err = expectThrows(() => yue.endless(), 'endless()');
    assert(err instanceof TypeError, 'endless() is a TypeError');
    err = expectThrows(() => yue.endless({ target: { style: FOLK } }), 'no vae');
    assert(err instanceof TypeError && /vae/.test(err.message), 'a missing vae is named: ' + err.message);
    err = expectThrows(() => yue.endless({ vae, paced: false }), 'unpaced without maxChunks');
    assert(err instanceof TypeError && /maxChunks/.test(err.message), 'maxChunks is named: ' + err.message);
    err = expectThrows(() => yue.endless({ vae, changeMode: 'sideways' }), 'a bad changeMode');
    assert(err instanceof TypeError && /changeMode/.test(err.message), 'changeMode is named: ' + err.message);
    err = expectThrows(() => yue.endless({ vae, path: [{ style: SYNTH }] }), 'a path entry without at');
    assert(err instanceof TypeError && /at/.test(err.message), 'the path\'s at is named: ' + err.message);
    err = expectThrows(() => yue.endless({ vae, whisper: {} }), 'a whisper that is not one');
    assert(err instanceof TypeError && /whisper/.test(err.message), 'whisper is named: ' + err.message);
    assert(!yue.busy && !vae.busy, 'a refused stream claims nothing');
}

function unpaced(yue, vae) {
    const events = [], chunks = [];
    let done = null;
    const st = yue.endless({
        vae, paced: false, maxChunks: 2, chunkData: true, seed: 3,
        target: { style: FOLK, tag: 'folk' },
        onAudio: (a) => chunks.push(a),
        onEvent: (e) => events.push(e),
        onDone: (i) => { done = i; },
    });
    assert(st.running && yue.busy && vae.busy, 'the stream runs and holds the model and the VAE');
    pumpUntil('the unpaced stream', () => done !== null, 180);
    assert(!done.cancelled && !done.error, 'onDone clean: ' + JSON.stringify(done));
    assert(!st.running && !yue.busy && !vae.busy, 'everything released after onDone');

    const sampled = events.filter((e) => e.kind === 'sampled'), built = events.filter((e) => e.kind === 'built');
    assert(sampled.length === 2 && built.length === 2, 'a sampled and a built per chunk');
    const lat = new Float32Array(500 * 64);
    for (let k = 0; k < 2; k++) {
        assert(sampled[k].chunk === k && sampled[k].tokens instanceof Int32Array && sampled[k].tokens.length === 250,
               'chunk ' + k + ': 250 tokens');
        assert(built[k].chunk === k && built[k].latents instanceof Float32Array && built[k].latents.length === 250 * 64 &&
               built[k].frame0 === k * 250, 'chunk ' + k + ': 250 x 64 latents at frame ' + k * 250);
        const r = built[k].row;
        assert(r.tag === 'folk' && r.index === k && r.memory === (k ? 250 : 0) && r.rendered && r.latentNorm > 6,
               'chunk ' + k + ' row: ' + JSON.stringify({ tag: r.tag, memory: r.memory, norm: r.latentNorm }));
        lat.set(built[k].latents, k * 250 * 64);
    }
    assert(events.some((e) => e.kind === 'done'), "a 'done' event");
    assert(events.filter((e) => e.kind === 'chunkStart').map((e) => e.chunk).join() === '0,1', 'chunkStart 0, 1');
    let at = 0, contiguous = true;
    for (const c of chunks) {
        if (c.sample0 !== at || c.channels !== 2 || c.sampleRate !== SR) contiguous = false;
        at += c.frames;
    }
    assert(contiguous, 'audio contiguous from 0, stereo, 48 kHz');
    const whole = vae.decode(lat);
    assert(at === whole.frames, 'program length ' + at + ' is the decode\'s ' + whole.frames);
    let md = 0, off = 0;
    for (const c of chunks) {
        for (let i = 0; i < c.frames; i++) {
            md = Math.max(md, Math.abs(c.samples[i] - whole.samples[off + i]),
                          Math.abs(c.samples[c.frames + i] - whole.samples[whole.frames + off + i]));
        }
        off += c.frames;
    }
    assert(md === 0, 'the program is vae.decode() of the latents exactly (max |d| ' + md + ')');
    const fit = bro.music.beat(whole, { bpm: 90 });
    console.log('unpaced: ' + (at / SR).toFixed(2) + ' s; the stream\'s beat at 90 bpm: contrast ' + fit.contrast.toFixed(2));
}

function paced(yue, vae) {
    const events = [];
    let emitted = 0, t0 = -1, done = null;
    const st = yue.endless({
        vae, seed: 5, target: { style: FOLK, tag: 'folk' },
        onAudio: (a) => { emitted = a.sample0 + a.frames; if (t0 < 0) t0 = Date.now(); },
        onEvent: (e) => events.push(e),
        onDone: (i) => { done = i; },
    });
    const play = () => {
        if (t0 >= 0) st.setPlayhead(Math.min(emitted, Math.floor((Date.now() - t0) * 48)));
    };
    pumpUntil('2 s of playback', () => { play(); return st.stats.playhead >= 2 * SR; }, 120);
    const s = st.stats;
    assert(typeof s.openChunk === 'number' && s.openSample === s.openChunk * 250 * 1920, 'openSample is openChunk\'s start');
    assert(s.target.tag === 'folk' && s.target.style === FOLK && s.target.score === null, 'stats.target: ' + JSON.stringify(s.target.tag));
    assert(Array.isArray(s.chunks) && Array.isArray(s.branches) && Array.isArray(s.gaps), 'stats lists');
    assert(['splice', 'render', 'decode', 'listen'].every((k) => typeof s.costSeconds[k] === 'number'), 'costSeconds');
    assert(s.emitted - s.playhead <= 1.0 * SR, 'at most the emit lead ahead');

    const merged = st.setTarget({ style: SYNTH, tag: 'synth' });
    assert(merged.style === SYNTH && merged.tag === 'synth' && merged.lyrics === '', 'setTarget returns the merged target');
    pumpUntil("the 'planned' event", () => { play(); return events.some((e) => e.kind === 'planned'); }, 10);
    const pl = events.find((e) => e.kind === 'planned');
    assert(pl.tag === 'synth' && (pl.chunk === s.openChunk || pl.chunk === s.openChunk + 1) &&
           pl.sample === pl.chunk * 250 * 1920, "planned at stats' open chunk: " + pl.chunk + ' / ' + s.openChunk);
    const k = st.setKnobs({ texture: 9, energy: 0.5 });
    assert(k.texture === 2 && k.energy === 0.5 && st.target.knobs.texture === 2, 'setKnobs clamps and merges: ' + JSON.stringify(k));
    pumpUntil("the 'knobs' event", () => { play(); return events.some((e) => e.kind === 'knobs'); }, 10);

    const id = st.branch({ style: FOLK, tag: 'other' });
    assert(id === 1, 'the first branch id is 1');
    pumpUntil("the 'branch' event", () => { play(); return events.some((e) => e.kind === 'branch'); }, 10);
    pumpUntil('the branch in stats', () => { play(); return st.stats.branches.some((b) => b.id === 1 && b.tag === 'other'); }, 10);
    st.discardBranch(id);
    pumpUntil("the 'discarded' event", () => { play(); return events.some((e) => e.kind === 'discarded' && e.branch === 1); }, 10);
    pumpUntil('the branch gone', () => { play(); return st.stats.branches.length === 0; }, 10);

    const before = st.stats;
    st.stop();
    pumpUntil('onDone after stop()', () => done !== null, 60);
    assert(!done.error, 'stop() ends cleanly: ' + JSON.stringify(done));
    assert(!yue.busy && !vae.busy, 'released after stop()');
    assert(!events.some((e) => e.kind === 'error'), 'no error events');
    console.log('paced: ' + (emitted / SR).toFixed(1) + ' s handed out; worst gap ' + before.worstGapSeconds.toFixed(3) +
                ' s; events ' + [...new Set(events.map((e) => e.kind))].join(' '));
}
