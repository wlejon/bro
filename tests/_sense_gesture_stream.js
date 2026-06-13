// 2b verification — per-stream sense + gesture tenants (model-free).
//
// Each stream gets its OWN SensorHub and GestureSpotter. Proves: independent
// sensing (two streams fed different audio report independent snapshots),
// independent gesture template sets, gesture rides the stream's own sense, and
// a self-feed fires on the right stream only. Run:
//   bro-headless tests/_smoke_app tests/_sense_gesture_stream.js

const RATE = 16000;

function silence(sec) { return new Float32Array(Math.floor(sec * RATE)); }
function tone(sec, hz, amp, t0) {
    const n = Math.floor(sec * RATE), fade = Math.floor(0.01 * RATE);
    const s = new Float32Array(n);
    for (let i = 0; i < n; i++) {
        const g = i >= n - fade ? (n - i) / fade : 1;
        s[i] = g * amp * Math.sin(2 * Math.PI * hz * (t0 + i / RATE));
    }
    return s;
}
function concat(...parts) {
    let n = 0; for (const p of parts) n += p.length;
    const out = new Float32Array(n); let o = 0;
    for (const p of parts) { out.set(p, o); o += p.length; }
    return out;
}
function feedChunks(feedFn, pcm) {
    const CHUNK = Math.floor(RATE / 100);   // 10 ms
    for (let off = 0; off < pcm.length; off += CHUNK)
        feedFn(pcm.subarray(off, Math.min(off + CHUNK, pcm.length)));
}
function pumpUntil(pred, budgetMs) {
    const start = Date.now();
    while (!pred() && (Date.now() - start) < budgetMs) sleep(20);
    return pred();
}

// ── 1. sense independence: two streams, different audio ───────────────────────
const s = bro.listen.open('mic');
assert(s && s.valid, 'second stream opened');
assert(s.sense && s.gesture, 'stream exposes .sense and .gesture views');

bro.sense.start({});         // default mic
s.sense.start({});           // second stream
assert(bro.sense.isActive() && s.sense.isActive(), 'both streams sensing');

const toneClip = concat(silence(0.3), tone(1.0, 1000, 0.2, 0.3));
let defSnap = null;
feedChunks((b) => { defSnap = bro.sense.feed(b); }, toneClip);   // tone -> default
let strSnap = null;
feedChunks((b) => { strSnap = s.sense.feed(b); }, silence(1.3)); // silence -> stream

assert(defSnap && defSnap.tonal, 'default mic heard a tone');
assert(Math.abs(defSnap.dominantHz - 1000) < 80,
       'default dominantHz near 1 kHz (' + defSnap.dominantHz.toFixed(0) + ')');
assert(strSnap && !strSnap.tonal, 'second stream (fed silence) is NOT tonal');
// Independent frame axes — each hub advanced only on its own feeds.
assert(bro.sense.snapshot().frames !== s.sense.snapshot().frames ||
       defSnap.tonal !== strSnap.tonal, 'streams have independent sensor state');
console.log('[sg-stream] sense independent: default tonal@1kHz, stream silent');

// ── 2. gesture: enroll on the stream, independent of default ───────────────────
const gestureClip = concat(silence(0.2), tone(1.2, 900, 0.25, 0.2), silence(0.2));
const beats = s.gesture.enrollFromAudio('whistle', gestureClip);
assert(beats >= 0, 'stream enrolled a gesture (beats=' + beats + ')');
assert(s.gesture.templates().indexOf('whistle') >= 0, 'stream lists the gesture');
assert(bro.gesture.templates().indexOf('whistle') < 0,
       'default gesture set does NOT contain the stream-only template');
console.log('[sg-stream] gesture templates independent: stream=[whistle], default=[]');

// ── 3. gesture fires on its own stream's feed, not the default ─────────────────
const strGestures = [], defGestures = [];
s.gesture.listen({ onGesture: (name, conf, kind) => strGestures.push({ name, conf, kind }) });
assert(s.gesture.isActive(), 'stream gesture listening');
assert(!bro.gesture.isActive(), 'default gesture not listening');

// Feed the enrolled clip through the STREAM (drives its sense + gesture members).
feedChunks((b) => s.sense.feed(b), gestureClip);

const fired = pumpUntil(() => strGestures.length > 0, 6000);
assert(fired, 'stream gesture fired on its own feed (' + JSON.stringify(strGestures) + ')');
assert(strGestures[0].name === 'whistle', 'fired the enrolled gesture');
assert(defGestures.length === 0, 'default gesture never fired (it was never fed/enrolled)');
console.log('[sg-stream] gesture fired on stream only: ' + JSON.stringify(strGestures[0]));

// ── 4. teardown: stop + close, default sense survives ──────────────────────────
s.gesture.stop();
s.sense.stop();
assert(!s.sense.isActive() && !s.gesture.isActive(), 'stream tenants stopped');
assert(bro.sense.isActive(), 'default mic still sensing after stream stop');
s.close();
assert(!s.valid, 'stream closed');
bro.sense.feed(silence(0.1));   // drive a tick-worth (prunes gesture tenants)
sleep(100);
assert(bro.sense.isActive(), 'default sense unaffected by stream close');

bro.sense.stop();
console.log('[sg-stream] PASS');
