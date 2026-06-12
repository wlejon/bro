// Smoke test for the shared listen host — bro.kws and bro.sense live at the
// same time on ONE stream (one tap, one ring, one PCEN front-end, one
// PhonemeNet forward). The cross-tenant assertions are the point:
//   - audio fed through bro.kws.feed advances bro.sense's sensors
//   - audio fed through bro.sense.feed fires bro.kws's onSpot
//   - stopping one tenant leaves the other's stream rolling
// Run against the minimal smoke app:
//   bro-headless tests/_smoke_app tests/_listen_smoke.js
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

// ── 1. both tenants up on the shared host ───────────────────────────────────

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
console.log('[smoke] both tenants live on the shared host');

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

// ── 4. shared tap diagnostics agree ─────────────────────────────────────────

const ks = bro.kws.stats(), ss = bro.sense.stats();
if (ks && ss) {
    assert(ks.framesDelivered === ss.framesDelivered,
           'both tenants report the SAME tap');
}

// ── 5. one tenant leaving does not disturb the other ────────────────────────

bro.sense.stop();
assert(!bro.sense.isActive(), 'sense stopped');
assert(bro.kws.isActive(), 'kws unaffected by sense leaving');
const again = feedChunks(phrase, (c) => bro.kws.feed(c));
assert(again.some((e) => e.name === 'hello-there'),
       'kws still spots after sense left (' + JSON.stringify(again) + ')');

bro.kws.stop();
bro.kws.unload();
assert(!bro.kws.isActive(), 'kws stopped');

console.log('[smoke] PASS');
