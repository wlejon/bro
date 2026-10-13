// yue.stream (YuE2Stream) through JS: streaming playback, the radio and
// steering the songs to come (docs/music-api.js). The stream's quality, latency and bug-guard
// oracles live in brosoundml's test_yue2_stream; this checks the binding.
//
// Weights-gated: the YuE2-3B folder ($YUE2_MODEL_DIR, else ../YuE2-3B) and
// the YuE2 VAE (BRO_WEIGHTS or ../brosoundml/weights /yue2-vae), on a GPU.
//   1. Argument errors are TypeErrors.
//   2. Unpaced, one 250-frame song (cot off, so no plan): the audio is
//      contiguous from sample 0 and is the decoder's natural length; the
//      events come in order; onLatents covers every frame; the model and
//      the VAE are busy while it runs and free after onDone.
//   3. Paced, with a simulated player: there is no interrupt(); setKnobs
//      clamps, is for the songs queued after (the song on air keeps its
//      own); stats has its shape (work included); stop() ends it.
//   4. aheadSongs 2, every song supplied on 'want': the pool fills with
//      built songs, skip() to one hands out its audio within 0.5 s, drop()
//      removes one and want asks again.
//   5. previewSeconds 10: previews are built ahead (rows say preview,
//      previewFrames 250), one nobody chose ends at 10 s, makeFull() on the
//      playing one gives a 'full' event and it goes on past its preview,
//      and setAutoFull(true) makes the next one on air full by itself.

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

const MODEL_DIR = process.env.YUE2_MODEL_DIR || '../YuE2-3B';
const VAE_DIR = (process.env.BRO_WEIGHTS || '../brosoundml/weights') + '/yue2-vae';

if (!bro.music || bro.music.available === false) {
    skipTest('bro.music is not in this build');
} else if (!fs.existsSync(MODEL_DIR + '/model.safetensors') || !fs.existsSync(VAE_DIR + '/model.safetensors')) {
    skipTest('needs ' + MODEL_DIR + ' and ' + VAE_DIR);
} else if (!bro.gpu || !bro.gpu.available) {
    skipTest('no GPU backend');
} else {
    const yue = bro.music.loadYuE2(MODEL_DIR);
    const vae = bro.music.loadVae(VAE_DIR, { decoderOnly: true });
    const request = JSON.parse(fs.readFileSync(MODEL_DIR + '/examples/tonight-awake.json', 'utf8'));
    const song = (frames, extra) => Object.assign({
        style: request.style, lyrics: request.lyrics, cot: 'off', seed: 5,
        semanticSampling: { minTokens: frames, maxTokens: frames },
    }, extra || {});
    argumentErrors(yue, vae);
    unpaced(yue, vae, song(250, { tag: 'u' }));
    paced(yue, vae, song(600, { tag: 'p' }));
    ahead(yue, vae, (n) => song(400, { tag: 'a' + n, seed: 40 + n }));
    previews(yue, vae, (n) => song(1000, { tag: 'v' + n, seed: 70 + n }));
}

function argumentErrors(yue, vae) {
    let err = expectThrows(() => yue.stream(), 'stream()');
    assert(err instanceof TypeError, 'stream() is a TypeError');
    err = expectThrows(() => yue.stream({}), 'stream({}) with no vae');
    assert(err instanceof TypeError && String(err.message).includes('vae'), 'a missing vae is named: ' + err.message);
    err = expectThrows(() => yue.stream({ vae, songs: [5] }), 'a song that is not an object');
    assert(err instanceof TypeError && String(err.message).includes('songs[0]'), 'the bad song is named: ' + err.message);
    err = expectThrows(() => yue.stream({ vae, songs: [{ cot: 'sideways' }] }), 'a bad cot');
    assert(err instanceof TypeError && String(err.message).includes('cot'), 'a bad cot is named: ' + err.message);
    err = expectThrows(() => yue.stream({ vae, aheadSongs: 9 }), 'aheadSongs 9');
    assert(err instanceof TypeError && String(err.message).includes('aheadSongs'), 'a bad aheadSongs is named: ' + err.message);
    err = expectThrows(() => yue.stream({ vae, previewSeconds: 5 }), 'previewSeconds 5');
    assert(err instanceof TypeError && String(err.message).includes('previewSeconds'), 'a bad previewSeconds is named: ' + err.message);
    assert(!yue.busy && !vae.busy, 'a refused stream claims nothing');
}

function unpaced(yue, vae, s) {
    const chunks = [], events = [], lat = new Uint8Array(250);
    let done = null;
    const st = yue.stream({
        vae, songs: [s], paced: false, radio: false,
        onAudio: (c) => chunks.push(c),
        onEvent: (e) => events.push(e),
        onLatents: (l) => { for (let f = l.frame0; f < l.frame0 + l.frames; f++) lat[f] = 1; },
        onDone: (info) => { done = info; },
    });
    assert(st.running, 'the stream runs');
    assert(yue.busy && vae.busy, 'the stream holds the model and the VAE');
    const err = expectThrows(() => yue.generate({ style: 'x', lyrics: 'y' }), 'generate during a stream');
    assert(String(err.message).includes('in flight'), 'generate during a stream is refused: ' + err.message);
    pumpUntil('the unpaced stream', () => done !== null, 180);
    assert(!done.cancelled && !done.error, 'onDone clean: ' + JSON.stringify(done));
    assert(!st.running && !yue.busy && !vae.busy, 'everything is released after onDone');

    let at = 0, contiguous = true, finite = true;
    for (const c of chunks) {
        if (c.sample0 !== at || c.channels !== 2 || c.sampleRate !== 48000) contiguous = false;
        for (let i = 0; i < c.samples.length; i += 97) if (!Number.isFinite(c.samples[i])) finite = false;
        at += c.frames;
    }
    assert(contiguous, 'audio chunks are contiguous from 0, stereo, 48 kHz');
    assert(finite, 'audio is finite');
    assert(at === vae.outputLength(250), 'program length ' + at + ' is the decoder\'s ' + vae.outputLength(250));
    assert(lat.every((x) => x === 1), 'onLatents covers all 250 frames');

    const kinds = events.map((e) => e.kind);
    const order = ['songStart', 'plan', 'semanticDone', 'firstAudio', 'songEnd', 'idle'];
    let k = 0;
    for (const want of order) {
        const i = kinds.indexOf(want, k);
        assert(i >= 0, 'event ' + want + ' in order (got ' + kinds.join(',') + ')');
        if (i >= 0) k = i;
    }
    const sd = events.find((e) => e.kind === 'semanticDone');
    assert(sd && sd.frames === 250 && sd.tag === 'u' && sd.song === 0, 'semanticDone: 250 frames, the tag, song 0');
    const end = events.find((e) => e.kind === 'songEnd');
    assert(end && end.sample === at, 'songEnd at the program end');
    assert(events.some((e) => e.kind === 'window' && e.value > 0), 'window events carry the latent norm');
    console.log('unpaced: ' + chunks.length + ' chunks, ' + (at / 48000).toFixed(2) + ' s; events ' + kinds.filter((x) => x !== 'window').join(' '));
}

function paced(yue, vae, s) {
    const events = [];
    let emitted = 0, done = null, t0 = -1, played = 0, moved = false, knobs = null;
    const st = yue.stream({
        vae, songs: [s], radio: false,
        onAudio: (c) => { emitted = c.sample0 + c.frames; if (t0 < 0) t0 = Date.now(); },
        onEvent: (e) => events.push(e),
        onDone: (info) => { done = info; },
    });
    const r = st.knobRanges;
    assert(r.energy.min === -2 && r.energy.max === 1 && r.percussion.max === 6, 'knobRanges: ' + JSON.stringify(r));
    assert(JSON.stringify(r) === JSON.stringify(bro.music.knobRanges),
           'a stream\'s knobRanges is bro.music.knobRanges: ' + JSON.stringify(bro.music.knobRanges));
    assert(typeof st.interrupt === 'undefined', 'no interrupt(): a song on air never changes');
    pumpUntil('the knobs event', () => {
        if (t0 >= 0) {
            played = Math.min(emitted, Math.floor((Date.now() - t0) * 48));
            st.setPlayhead(played);
        }
        if (!moved && played >= 4 * 48000) {
            moved = true;
            knobs = st.setKnobs({ texture: 9 });
        }
        return events.some((e) => e.kind === 'knobs');
    }, 120);
    assert(knobs && knobs.texture === 2 && knobs.energy === 0, 'setKnobs clamps and keeps the rest: ' + JSON.stringify(knobs));
    const kn = events.find((e) => e.kind === 'knobs');
    assert(kn.text === 'texture' && kn.song === -1, 'the knobs event names what moved and no song: ' + JSON.stringify(kn));
    const stats = st.stats;
    assert(stats.songs.length === 1 && stats.songs[0].tag === 'p' && stats.songs[0].start === 0, 'stats.songs: ' + JSON.stringify(stats.songs));
    assert(stats.knobs.texture === 2 && stats.songs[0].knobs.texture === 0,
           'the next song takes the knob, the one on air keeps its own: ' + JSON.stringify(stats.songs[0].knobs));
    assert(stats.emitted >= stats.playhead, 'stats frontier');
    assert(stats.work && typeof stats.work === 'object' && Object.values(stats.work).every((v) => v >= 0),
           'stats.work: seconds per piece of work: ' + JSON.stringify(stats.work));
    assert(stats.emitted - stats.playhead <= 0.9 * 48000, 'paced: at most the emit lead ahead (' + (stats.emitted - stats.playhead) + ')');
    st.stop();
    pumpUntil('onDone after stop()', () => done !== null, 30);
    assert(!done.error, 'stop() ends cleanly: ' + JSON.stringify(done));
    assert(!yue.busy && !vae.busy, 'released after stop()');
}

// Two songs kept built ahead, every song supplied through 'want': the first
// want asks for the playing song and two more; the pool fills with built
// songs; skip() to a built one hands out its audio at once; drop() removes
// one and want asks again; a song's stats row carries its knobs.
function ahead(yue, vae, make) {
    const events = [];
    let n = 0, emitted = 0, done = null, t0 = -1, played = 0, st = null;
    st = yue.stream({
        vae, aheadSongs: 2, radio: false,
        onAudio: (c) => { emitted = c.sample0 + c.frames; if (t0 < 0) t0 = Date.now(); },
        onEvent: (e) => {
            events.push(e);
            if (e.kind === 'want') for (let i = 0; i < e.value; i++) st.enqueue(make(n++));
        },
        onDone: (info) => { done = info; },
    });
    const play = () => {
        if (t0 < 0) return;
        played = Math.min(emitted, Math.floor((Date.now() - t0) * 48));
        st.setPlayhead(played);
    };
    // Ready (its opening decoded) comes before built (wholly decoded): wait for both.
    pumpUntil('two songs built ahead', () => {
        play();
        const t = st.stats;
        return t.readyAhead >= 2 && t.songs.filter((g) => !g.onAir && g.built).length >= 2;
    }, 180);
    const wants = events.filter((e) => e.kind === 'want');
    assert(wants.length >= 1 && wants[0].value === 3, 'the first want asks for 3: ' + JSON.stringify(wants[0]));
    let s = st.stats;
    const waiting = s.songs.filter((g) => !g.onAir);
    assert(s.ahead === 2 && waiting.length === 2 && waiting.every((g) => g.built && g.ready),
           'two built songs wait: ' + JSON.stringify(s.songs.map((g) => [g.index, g.built, g.ready, g.onAir])));
    assert(s.hostBytes > 0 && waiting.every((g) => g.hostBytes > 0), 'host memory reported: ' + s.hostBytes);
    assert(events.filter((e) => e.kind === 'built').length >= 2, 'a built event per song built');
    assert(typeof waiting[0].knobs.texture === 'number' && waiting[0].knobs.arrangement === 0, 'a row carries its knobs');

    // Skip to the first waiting song: its audio is handed out at once.
    const to = waiting[0].index, t = Date.now();
    st.skip();
    pumpUntil('the skip', () => { play(); return events.some((e) => e.kind === 'firstAudio' && e.song === to); }, 30);
    const fa = events.find((e) => e.kind === 'firstAudio' && e.song === to);
    const start = events.filter((e) => e.kind === 'songStart' && e.song === to).pop();
    console.log('ahead: skip to built song ' + to + ': cut over ' + start.seconds.toFixed(3) + ' s, first audio ' +
                fa.seconds.toFixed(3) + ' s after the request (JS saw it ' + ((Date.now() - t) / 1000).toFixed(2) + ' s later); host ' +
                (s.hostBytes / 1e6).toFixed(1) + ' MB');
    assert(fa.seconds < 0.5, 'skip to a built song: first audio ' + fa.seconds + ' s after the request');

    // Drop the last song queued: 'dropped', then a want refills the pool.
    s = st.stats;
    const last = s.songs[s.songs.length - 1].index;
    const before = events.filter((e) => e.kind === 'want').length;
    st.drop(last);
    pumpUntil('the drop', () => { play(); return events.some((e) => e.kind === 'dropped' && e.song === last); }, 30);
    pumpUntil('a want after the drop', () => { play(); return events.filter((e) => e.kind === 'want').length > before; }, 30);
    assert(!st.stats.songs.some((g) => g.index === last), 'the dropped song left the queue');
    st.stop();
    pumpUntil('onDone after stop()', () => done !== null, 30);
    assert(!done.error, 'stop() ends cleanly: ' + JSON.stringify(done));
}

// Previews of 10 s (250 frames), two kept built ahead, each song a 40 s one
// (1000 tokens) supplied on 'want'.
function previews(yue, vae, make) {
    const events = [];
    let n = 0, emitted = 0, done = null, t0 = -1, st = null;
    st = yue.stream({
        vae, aheadSongs: 2, radio: false, previewSeconds: 10,
        onAudio: (c) => { emitted = c.sample0 + c.frames; if (t0 < 0) t0 = Date.now(); },
        onEvent: (e) => {
            events.push(e);
            if (e.kind === 'want') for (let i = 0; i < e.value; i++) st.enqueue(make(n++));
        },
        onDone: (info) => { done = info; },
    });
    const play = () => {
        if (t0 < 0) return;
        st.setPlayhead(Math.min(emitted, Math.floor((Date.now() - t0) * 48)));
    };
    const row = (i) => st.stats.songs.find((g) => g.index === i);
    pumpUntil('two previews built ahead', () => {
        play();
        return st.stats.songs.filter((g) => !g.onAir && g.built && g.preview).length >= 2;
    }, 180);
    const waiting = st.stats.songs.filter((g) => !g.onAir);
    for (const g of waiting.filter((x) => x.built))
        assert(g.preview && g.previewFrames === 250 && g.decodedFrames === 250 && g.semanticFrames >= 250,
               'a built preview: ' + JSON.stringify([g.index, g.preview, g.previewFrames, g.semanticFrames, g.decodedFrames]));

    // Nobody chose song 0: it ends at its preview's 10 s.
    pumpUntil('song 0 ends', () => { play(); return events.some((e) => e.kind === 'songEnd' && e.song === 0); }, 60);
    const s0 = events.find((e) => e.kind === 'songStart' && e.song === 0);
    const e0 = events.find((e) => e.kind === 'songEnd' && e.song === 0);
    const len0 = (e0.sample - s0.sample) / 48000;
    assert(len0 > 9.5 && len0 < 10.5, 'an unchosen preview plays its 10 s: ' + len0);

    // Make the one on air full, 2 s into it.
    const air = st.stats.songs.find((g) => g.onAir);
    pumpUntil('2 s into song ' + air.index, () => { play(); return st.stats.playhead >= air.start + 2 * 48000; }, 30);
    st.makeFull();
    pumpUntil('the full event', () => { play(); return events.some((e) => e.kind === 'full' && e.song === air.index); }, 30);
    const full = events.find((e) => e.kind === 'full' && e.song === air.index);
    assert(full.value === 1 && full.frame0 > 0 && full.frame0 <= 250, 'made full, joined at frame ' + full.frame0);
    pumpUntil('the song past its preview', () => {
        play();
        const g = row(air.index);
        return g && !g.preview && g.latentFrames > 300;
    }, 60);
    console.log('previews: song 0 played ' + len0.toFixed(2) + ' s; song ' + air.index + ' made full, joined at ' +
                (full.frame0 / 25).toFixed(2) + ' s');

    // Auto full: the next song on air is made full by itself.
    st.setAutoFull(true);
    st.skip();
    pumpUntil('an auto full', () => {
        play();
        return events.some((e) => e.kind === 'full' && e.song > air.index && e.value === 1);
    }, 60);
    st.stop();
    pumpUntil('onDone after stop()', () => done !== null, 30);
    assert(!done.error, 'stop() ends cleanly: ' + JSON.stringify(done));
}
