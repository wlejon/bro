// Headless smoke test for bro.mic — drives broaudio's chunkFrames path with
// synthetic audio and asserts the fixed-size framing, stats, and AGC behaviour.
//
// Run from the bro repo root (against the neutral launcher app):
//   ./build/Debug/bro-headless.exe ../broworkshop tests/smoke_mic_chunks.js
//
// No audio device is opened (live:false); bro.mic.feed injects samples through
// the same tap the live audio thread would, exercising resample → AGC → chunk.
// The companion windowed demo lives at ../broworkshop/demos/mic-chunks.

function assert(cond, msg) { if (!cond) throw new Error('FAIL: ' + msg); }
function approx(a, b, tol, msg) {
  assert(Math.abs(a - b) <= tol, `${msg} (got ${a}, want ${b} ±${tol})`);
}

const ENGINE_RATE = bro.mic.engineRate();

function tone(durSec, hz, rate, peak) {
  const n = Math.floor(durSec * rate);
  const out = new Float32Array(n);
  const w = 2 * Math.PI * hz / rate;
  for (let i = 0; i < n; i++) out[i] = peak * Math.sin(w * i);
  return out;
}

let passed = 0;
function ok(name) { passed++; console.log('  ok - ' + name); }

// ── 1. Exact chunk count at the engine rate (no resampler) ───────────────────
// targetRate:0 means deliver at the engine's native rate, so chunk math is
// exact: floor(totalSamples / chunkFrames).
{
  const CHUNK = 441;            // 10 ms at 44100
  bro.mic.start({ chunkFrames: CHUNK, targetRate: 0, agc: false, live: false });

  const sig = tone(1.0, 440, ENGINE_RATE, 0.5);   // 1 s of audio
  bro.mic.feed(sig, ENGINE_RATE);

  const s = bro.mic.stats();
  assert(s, 'stats() returned null');
  const expected = Math.floor(sig.length / CHUNK);
  assert(s.chunkCount === expected,
    `chunkCount ${s.chunkCount} !== expected ${expected}`);
  assert(s.framesDelivered === expected,
    `framesDelivered ${s.framesDelivered} !== ${expected}`);
  assert(s.samplesDelivered === expected * CHUNK,
    `samplesDelivered ${s.samplesDelivered} !== ${expected * CHUNK}`);
  assert(s.chunkFrames === CHUNK, 'chunkFrames echoed wrong');
  assert(s.dropped === 0, 'unexpected drops');

  // Each chunk's peak should be ~0.5 (the tone amplitude), AGC off.
  const lv = bro.mic.levels(16);
  assert(lv.length > 0, 'no levels');
  for (const p of lv) approx(p, 0.5, 0.08, 'chunk peak (no AGC)');

  bro.mic.stop();
  ok('exact chunk count at engine rate, peaks unmodified');
}

// ── 2. Resampling path: 44100 → 16000, 160-frame chunks ──────────────────────
{
  const CHUNK = 160;            // 10 ms at 16000
  bro.mic.start({ chunkFrames: CHUNK, targetRate: 16000, agc: false, live: false });

  const sig = tone(1.0, 440, ENGINE_RATE, 0.5);
  bro.mic.feed(sig, ENGINE_RATE);

  const s = bro.mic.stats();
  // 1 s of audio → ~16000 resampled samples → ~100 chunks. Resampler latency
  // and the trailing partial chunk cost a few, so allow a small band.
  approx(s.chunkCount, 100, 4, 'resampled chunk count');
  assert(s.chunkFrames === CHUNK, 'chunkFrames echoed wrong');

  bro.mic.stop();
  ok('resampled 44.1k->16k yields ~100 chunks/sec of 160 frames');
}

// ── 3. AGC lifts a quiet signal toward the target peak ───────────────────────
{
  bro.mic.start({
    chunkFrames: 160, targetRate: 16000, live: false,
    agc: true, targetPeak: 0.95, halfLifeSec: 0.1, noiseGate: 0.001, maxGain: 100,
  });

  // Feed several seconds of quiet tone so the AGC converges.
  for (let k = 0; k < 4; k++) {
    bro.mic.feed(tone(1.0, 440, ENGINE_RATE, 0.05), ENGINE_RATE);
  }

  const lv = bro.mic.levels(8);   // most recent chunks, after convergence
  let maxPeak = 0;
  for (const p of lv) if (p > maxPeak) maxPeak = p;
  assert(maxPeak > 0.5,
    `AGC should lift 0.05 input well above 0.5 (got peak ${maxPeak.toFixed(3)})`);

  bro.mic.stop();
  ok('AGC lifts quiet input toward target peak');
}

// ── 4. feed() guards against use without an active tap ───────────────────────
{
  let threw = false;
  try { bro.mic.feed(new Float32Array(160)); }
  catch (e) { threw = true; }
  assert(threw, 'feed() before start() should throw');
  ok('feed() guards against use without an active tap');
}

console.log(`\nbro.mic: ${passed}/4 checks passed`);
