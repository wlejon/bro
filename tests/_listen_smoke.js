// Smoke test for the shared listen host — bro.kws, bro.sense, AND bro.wake
// live at the same time on ONE stream (one raw no-AGC tap, one ring, one
// PCEN front-end, one forward per model). The cross-tenant assertions are
// the point:
//   - audio fed through bro.kws.feed advances bro.sense's sensors
//   - audio fed through bro.sense.feed fires bro.kws's onSpot
//   - a "computer" utterance fed through bro.sense.feed fires bro.wake
//   - stopping one tenant leaves the others' stream rolling
// Run against the minimal smoke app:
//   bro-headless tests/_smoke_app tests/_listen_smoke.js
// Needs the Kokoro weights, a trained PhonemeNet checkpoint (.bpm), and the
// trained wake checkpoint (.bw).

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
const WAKE_WEIGHTS = '../brosoundml/weights/wake/computer.bw';
assert(fs.existsSync(WAKE_WEIGHTS), 'wake checkpoint exists (' + WAKE_WEIGHTS + ')');

const RATE = 16000;

// ── helpers ──────────────────────────────────────────────────────────────────

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

function speak(text) {
    const res = kokoro.synthesize(bro.tts.phonemize(text), voice);
    return resampleTo(res.samples, res.sampleRate, RATE);
}

function silence(sec) { return new Float32Array(Math.floor(sec * RATE)); }

// 10 ms fade-out — a hard mid-cycle cutoff is a real broadband transient.
function tone(sec, hz, amp) {
    const n = Math.floor(sec * RATE), fade = Math.floor(0.01 * RATE);
    const s = new Float32Array(n);
    for (let i = 0; i < n; i++) {
        const g = i >= n - fade ? (n - i) / fade : 1;
        s[i] = g * amp * Math.sin(2 * Math.PI * hz * i / RATE);
    }
    return s;
}

function concat(...parts) {
    let n = 0;
    for (const p of parts) n += p.length;
    const out = new Float32Array(n);
    let o = 0;
    for (const p of parts) { out.set(p, o); o += p.length; }
    return out;
}

// Feed `all` in 100 ms chunks through `feedFn`, collecting any kws events the
// feed itself reports (bro.sense.feed reports snapshots, not events — those
// arrive via onSpot instead).
function feedChunks(all, feedFn) {
    const events = [];
    const CHUNK = Math.floor(RATE / 10);
    for (let off = 0; off < all.length; off += CHUNK) {
        const got = feedFn(all.subarray(off, Math.min(off + CHUNK, all.length)));
        if (Array.isArray(got)) events.push(...got);
    }
    return events;
}

// ── 1. all three tenants up on the shared host ──────────────────────────────

bro.sense.start({});
assert(bro.sense.isActive(), 'sense active');

bro.kws.load({ weights: KWS_WEIGHTS });
const ids = bro.tts.phonemize('hello there');
const tlen = bro.kws.enroll('hello-there', ids);
assert(tlen >= 3, 'template enrolled (len ' + tlen + ')');

const spots = [];
bro.kws.listen({ onSpot: (name, confidence) => spots.push({ name, confidence }) });
assert(bro.kws.isActive(), 'kws listening');
assert(bro.sense.isActive(), 'sense still active after kws joined');

let wakeFires = 0;
bro.wake.listen({
    weights:   WAKE_WEIGHTS,
    threshold: 0.85,
    onFire:    () => { wakeFires++; },
});
assert(bro.wake.isActive(), 'wake listening');
assert(bro.kws.isActive() && bro.sense.isActive(),
       'kws + sense unaffected by wake joining');
console.log('[smoke] all three tenants live on the shared host');

// ── 2. cross-tenant: feed a tone through bro.kws.feed, sense must see it ────

const senseBefore = bro.sense.snapshot();
feedChunks(concat(silence(0.3), tone(1.0, 1000, 0.1)), (c) => bro.kws.feed(c));
const senseAfter = bro.sense.snapshot();
assert(senseAfter.frames > senseBefore.frames,
       'sense frames advanced via bro.kws.feed (' +
       senseBefore.frames + ' -> ' + senseAfter.frames + ')');
assert(senseAfter.tonalFrames > 50,
       'sense heard the tone fed through kws (tonalFrames=' +
       senseAfter.tonalFrames + ')');
assert(Math.abs(senseAfter.dominantHz - 1000) < 70,
       'dominantHz near 1 kHz (' + senseAfter.dominantHz.toFixed(0) + ')');
console.log('[smoke] tone fed via bro.kws.feed -> sense tonalFrames=' +
            senseAfter.tonalFrames);

// ── 3. cross-tenant: feed speech through bro.sense.feed, kws must fire ──────

const phrase = concat(silence(0.3), speak('hello there'), silence(0.3));
feedChunks(phrase, (c) => bro.sense.feed(c));
assert(pumpUntil(() => spots.some((s) => s.name === 'hello-there'), 10000),
       'kws onSpot fired from audio fed via bro.sense.feed');
const sVoice = bro.sense.snapshot();
assert(sVoice.voiceEvents >= 1, 'sense counted the speech as voice too');
console.log('[smoke] phrase fed via bro.sense.feed -> kws spotted it (conf ' +
            spots.find((s) => s.name === 'hello-there').confidence.toFixed(3) + ')');

// ── 4. cross-tenant: "computer" through bro.sense.feed fires bro.wake ───────
//
// The wake model rode the same single stream the whole time: the tone, the
// "hello there" phrase, and now its own keyword — all one PCEN front-end.
// The AGC-free model is level-invariant, so the raw Kokoro level (no AGC,
// no normalization) must fire it as-is.

assert(wakeFires === 0, 'wake silent through tone + non-keyword speech');
const computer = concat(silence(0.4), speak('computer'), silence(0.3));
feedChunks(computer, (c) => bro.sense.feed(c));
assert(pumpUntil(() => wakeFires >= 1, 10000),
       'wake fired from audio fed via bro.sense.feed');
console.log('[smoke] "computer" fed via bro.sense.feed -> wake fired (score max ' +
            bro.wake.stats().scoreMax.toFixed(3) + ')');

// ── 5. shared tap diagnostics agree ─────────────────────────────────────────

const ks = bro.kws.stats(), ss = bro.sense.stats(), ws = bro.wake.stats();
if (ks && ss && ws) {
    assert(ks.framesDelivered === ss.framesDelivered &&
           ks.framesDelivered === ws.framesDelivered,
           'all three tenants report the SAME tap');
}

// ── 6. one tenant leaving does not disturb the others ───────────────────────

bro.wake.stop();
assert(!bro.wake.isActive(), 'wake stopped');
assert(bro.kws.isActive() && bro.sense.isActive(),
       'kws + sense unaffected by wake leaving');

bro.sense.stop();
assert(!bro.sense.isActive(), 'sense stopped');
assert(bro.kws.isActive(), 'kws unaffected by sense leaving');
const again = feedChunks(phrase, (c) => bro.kws.feed(c));
assert(again.some((e) => e.name === 'hello-there'),
       'kws still spots after wake + sense left (' + JSON.stringify(again) + ')');

bro.kws.stop();
bro.kws.unload();
assert(!bro.kws.isActive(), 'kws stopped');

console.log('[smoke] PASS');
