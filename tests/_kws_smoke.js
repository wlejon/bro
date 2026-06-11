// Smoke test for bro.kws — open-vocabulary streaming keyword spotting
// (brosoundml::PhonemeSpotter). End-to-end on synthesized speech: enroll a
// phrase from bro.tts.phonemize ids, synthesize the same phrase with Kokoro,
// feed it through the spotter, and expect a named spot event — both from
// feed()'s synchronous return (headless path) and via the onSpot tick
// delivery. Also exercises enrollFromAudio, suspend/resume gating, and the
// enroll-while-listening guard.
// Run against the minimal smoke app:
//   bro-headless tests/_smoke_app tests/_kws_smoke.js
// Needs the Kokoro weights and a trained PhonemeNet checkpoint (.bpm).

function pumpUntil(pred, budgetMs) {
    const start = Date.now();
    while (!pred() && (Date.now() - start) < budgetMs) { sleep(20); }
    return pred();
}

const fs = require('fs');

const KOKORO_DIR = '../brosoundml/weights/kokoro';
const VOICE_PATH = '../brosoundml/weights/kokoro/voices/af_bella.bin';
const KWS_CANDIDATES = [
    '../brosoundml/weights/phoneme/english.bpm',
    '../brosoundml/build-cuda/english.bpm',
];
const KWS_WEIGHTS = KWS_CANDIDATES.find((p) => fs.existsSync(p));
assert(KWS_WEIGHTS, 'a PhonemeNet checkpoint exists (' + KWS_CANDIDATES.join(', ') + ')');

// ── synthesis helpers ────────────────────────────────────────────────────────

const kokoro = bro.tts.loadKokoro(KOKORO_DIR);
const voice  = kokoro.loadVoice(VOICE_PATH);

// Kokoro speaks at 24 kHz; the spotter wants its model rate (16 kHz).
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

// Feed a clip in 100 ms chunks with leading/trailing silence (the matcher's
// entry gate needs silence frames before a word may begin). Returns every
// event feed() reported.
function feedClip(clip, rate) {
    const silence = new Float32Array(Math.floor(rate * 0.3));
    const all = new Float32Array(silence.length * 2 + clip.length);
    all.set(silence, 0);
    all.set(clip, silence.length);
    all.set(silence, silence.length + clip.length);
    const events = [];
    const CHUNK = Math.floor(rate / 10);
    for (let off = 0; off < all.length; off += CHUNK) {
        const got = bro.kws.feed(all.subarray(off, Math.min(off + CHUNK, all.length)));
        if (Array.isArray(got)) events.push(...got);
    }
    return events;
}

// ── 1. load + enroll ─────────────────────────────────────────────────────────

bro.kws.load({ weights: KWS_WEIGHTS });
assert(bro.kws.isLoaded(), 'spotter loaded');
const rate = bro.kws.sampleRate();
assert(rate === 16000, 'spotter sample rate 16000 (got ' + rate + ')');

const ids = bro.tts.phonemize('hello there');
assert(ids.length > 0, 'phonemize yielded ids');
const tlen = bro.kws.enroll('hello-there', ids);
assert(tlen >= 3, 'template has >= 3 phoneme classes (got ' + tlen + ')');
assert(bro.kws.templates().indexOf('hello-there') >= 0, 'template listed');
console.log('[smoke] kws loaded (' + KWS_WEIGHTS + '), template len ' + tlen);

// ── 2. listen + spot the synthesized phrase ──────────────────────────────────

const spots = [];
bro.kws.listen({ onSpot: (name, confidence) => spots.push({ name, confidence }) });
assert(bro.kws.isActive(), 'listening');

// Mutators are rejected while the inference thread owns feed().
let threw = false;
try { bro.kws.enroll('nope', ids); } catch (e) { threw = true; }
assert(threw, 'enroll while listening throws');

const positive = speak('hello there', rate);
assert(positive.length > rate / 4, 'synthesized clip is non-trivial');
const events = feedClip(positive, rate);
assert(events.some((e) => e.name === 'hello-there'),
       'feed() reported the enrolled phrase (' + JSON.stringify(events) + ')');
const conf = events.find((e) => e.name === 'hello-there').confidence;
assert(conf > 0 && conf <= 1, 'confidence in (0,1] (got ' + conf + ')');

assert(pumpUntil(() => spots.some((s) => s.name === 'hello-there'), 10000),
       'onSpot delivered on the main thread');
console.log('[smoke] spotted "hello there" (confidence ' + conf.toFixed(3) + ')');

// ── 3. negative phrase (report, no hard assert — open-vocab FA is a tuning
//       property, not a binding property) ────────────────────────────────────
const negativeEvents = feedClip(speak('banana smoothie recipe', rate), rate);
console.log('[smoke] negative phrase events: ' + JSON.stringify(negativeEvents));

// ── 4. suspend gates onSpot delivery, feed still reports ─────────────────────
bro.kws.suspend();
assert(bro.kws.isSuspended(), 'suspended');
const spotsBefore = spots.length;
const suspendedEvents = feedClip(positive, rate);
sleep(200);
assert(spots.length === spotsBefore,
       'no onSpot delivery while suspended (got ' + (spots.length - spotsBefore) + ')');
assert(suspendedEvents.some((e) => e.name === 'hello-there'),
       'feed() still reports events while suspended');
bro.kws.resume();
assert(!bro.kws.isSuspended(), 'resumed');

// ── 5. enrollFromAudio: reference-audio template fires on its own clip ──────
bro.kws.stop();
assert(!bro.kws.isActive(), 'stopped');
const refClip = speak('open the pod bay doors', rate);
const len2 = bro.kws.enrollFromAudio('pod-bay', refClip);
assert(len2 >= 3, 'audio-enrolled template len >= 3 (got ' + len2 + ')');

const spots2 = [];
bro.kws.listen({ onSpot: (name, confidence) => spots2.push({ name, confidence }) });
const events2 = feedClip(refClip, rate);
assert(events2.some((e) => e.name === 'pod-bay'),
       'audio-enrolled template fired (' + JSON.stringify(events2) + ')');

// ── 6. teardown ──────────────────────────────────────────────────────────────
bro.kws.stop();
bro.kws.unload();
assert(!bro.kws.isLoaded(), 'unloaded');

console.log('[smoke] PASS');
