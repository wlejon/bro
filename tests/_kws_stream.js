// 2b verification — per-stream kws tenants over ONE shared net.
//
// bro.kws.load() loads the PhonemeNet once; the default-mic stream (bro.kws.*)
// and a second opened stream (stream.kws.*) each get their OWN spotter over the
// shared weights. Proves: independent template sets, crosstalk-free spotting
// (each stream only fires on what IT was fed + enrolled), and tenant teardown
// on stream close. Run:
//   bro-headless tests/_smoke_app tests/_kws_stream.js
// Needs the Kokoro weights and a trained PhonemeNet checkpoint (.bpm).

function pumpUntil(pred, budgetMs) {
    const start = Date.now();
    while (!pred() && (Date.now() - start) < budgetMs) { sleep(20); }
    return pred();
}

const fs = require('fs');
const KOKORO_DIR = '../brosoundml/weights/kokoro';
const VOICE_PATH = '../brosoundml/weights/kokoro/voices/af_bella.bin';
const KWS_WEIGHTS = [
    '../brosoundml/weights/phoneme/english.bpm',
    '../brosoundml/build-cuda/english.bpm',
].find((p) => fs.existsSync(p));
assert(KWS_WEIGHTS, 'a PhonemeNet checkpoint exists');

const kokoro = bro.tts.loadKokoro(KOKORO_DIR);
const voice  = kokoro.loadVoice(VOICE_PATH);

function resampleTo(samples, fromRate, toRate) {
    if (fromRate === toRate) return samples;
    const ratio = toRate / fromRate, m = Math.floor(samples.length * ratio);
    const out = new Float32Array(m);
    for (let i = 0; i < m; i++) {
        const t = i / ratio, j = t | 0, f = t - j;
        out[i] = samples[j] * (1 - f) +
                 (samples[j + 1] !== undefined ? samples[j + 1] : samples[j]) * f;
    }
    return out;
}
function speak(text, rate) {
    const res = kokoro.synthesize(bro.tts.phonemize(text), voice);
    return resampleTo(res.samples, res.sampleRate, rate);
}

// Feed a clip (with entry-gate silence padding) through `feedFn`, collecting the
// events feedFn returns synchronously (headless inline path).
function feedClip(feedFn, clip, rate) {
    const silence = new Float32Array(Math.floor(rate * 0.3));
    const all = new Float32Array(silence.length * 2 + clip.length);
    all.set(silence, 0);
    all.set(clip, silence.length);
    all.set(silence, silence.length + clip.length);
    const events = [];
    const CHUNK = Math.floor(rate / 10);
    for (let off = 0; off < all.length; off += CHUNK) {
        const got = feedFn(all.subarray(off, Math.min(off + CHUNK, all.length)));
        if (Array.isArray(got)) events.push(...got);
    }
    return events;
}

// ── load once ────────────────────────────────────────────────────────────────
bro.kws.load({ weights: KWS_WEIGHTS });
assert(bro.kws.isLoaded(), 'net loaded');
const rate = bro.kws.sampleRate();
assert(rate === 16000, 'sample rate 16000');

// ── open a second, independent stream ─────────────────────────────────────────
const s = bro.listen.open('mic');           // its own source/ring/bus/spotter
assert(s && s.valid, 'second stream opened');
assert(s.kws, 'stream exposes a .kws view');
assert(!s.kws.isActive(), 'stream not yet listening');
// isLoaded reflects the SHARED net through either home.
assert(s.kws.isLoaded(), 'stream sees the shared net as loaded');

// ── independent template sets over shared weights ─────────────────────────────
bro.kws.enroll('alpha', bro.tts.phonemize('hello there'));      // default mic
s.kws.enroll('beta',    bro.tts.phonemize('good morning'));     // second stream

const defTpls = bro.kws.templates();
const strTpls = s.kws.templates();
assert(defTpls.length === 1 && defTpls[0] === 'alpha',
       'default-mic templates == [alpha] (got ' + JSON.stringify(defTpls) + ')');
assert(strTpls.length === 1 && strTpls[0] === 'beta',
       'stream templates == [beta] (got ' + JSON.stringify(strTpls) + ')');
console.log('[stream] independent templates: default=[alpha] stream=[beta]');

// ── listen on both, with separate sinks ───────────────────────────────────────
const defSpots = [], strSpots = [];
bro.kws.listen({ onSpot: (name, conf) => defSpots.push({ name, conf }) });
s.kws.listen({ onSpot: (name, conf) => strSpots.push({ name, conf }) });
assert(bro.kws.isActive() && s.kws.isActive(), 'both streams listening');

// Mutator guard is per-stream: stream B is listening, so its enroll is rejected,
// independent of A.
let threw = false;
try { s.kws.enroll('nope', bro.tts.phonemize('nope')); } catch (e) { threw = true; }
assert(threw, 'enroll on a listening stream throws');

// ── crosstalk check: feed each stream its OWN phrase ───────────────────────────
const hello = speak('hello there', rate);
const morning = speak('good morning', rate);

const defEvents = feedClip((b) => bro.kws.feed(b), hello, rate);
const strEvents = feedClip((b) => s.kws.feed(b), morning, rate);

assert(defEvents.some((e) => e.name === 'alpha'),
       'default mic fired alpha on "hello there" (' + JSON.stringify(defEvents) + ')');
assert(!defEvents.some((e) => e.name === 'beta'),
       'default mic never fires beta (stream-only template)');
assert(strEvents.some((e) => e.name === 'beta'),
       'stream fired beta on "good morning" (' + JSON.stringify(strEvents) + ')');
assert(!strEvents.some((e) => e.name === 'alpha'),
       'stream never fires alpha (default-only template)');
console.log('[stream] crosstalk-free: alpha@default, beta@stream, no leakage');

// onSpot delivery routes to the right sink.
assert(pumpUntil(() => defSpots.some((x) => x.name === 'alpha'), 8000),
       'default onSpot delivered alpha');
assert(pumpUntil(() => strSpots.some((x) => x.name === 'beta'), 8000),
       'stream onSpot delivered beta');
assert(!defSpots.some((x) => x.name === 'beta'), 'default sink never saw beta');
assert(!strSpots.some((x) => x.name === 'alpha'), 'stream sink never saw alpha');
console.log('[stream] onSpot routing isolated per stream');

// ── close the stream → its tenant is pruned, default mic unaffected ────────────
s.kws.stop();
assert(!s.kws.isActive(), 'stream stopped');
assert(bro.kws.isActive(), 'default mic still listening after stream stop');
s.close();
assert(!s.valid, 'stream closed');
// A tick prunes the closed stream's tenant; the default-mic tenant survives.
bro.kws.feed(new Float32Array(Math.floor(rate * 0.1)));   // drive a tick-worth
sleep(100);
assert(bro.kws.isLoaded(), 'shared net still loaded after one stream closed');

bro.kws.stop();
bro.kws.unload();
assert(!bro.kws.isLoaded(), 'unloaded');

console.log('[stream] PASS');
