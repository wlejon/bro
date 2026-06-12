// bro.sense headless smoke test — synthetic audio through the live binding.
// Run: bro-headless tests/_smoke_app tests/_sense_smoke.js
// Headless has no mic capture, so start() registers the tap (inert) and
// feed() runs the hub synchronously, returning the post-feed snapshot.

const RATE = 16000;

function silence(sec) { return new Float32Array(Math.floor(sec * RATE)); }

// 10 ms fade-out: a hard mid-cycle cutoff is a REAL broadband transient (the
// onset sensor fires on it, correctly), so the tone must end cleanly for the
// "no onsets in silence" check to mean what it says.
function tone(sec, hz, amp, t0) {
  const n = Math.floor(sec * RATE);
  const fade = Math.floor(0.01 * RATE);
  const s = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    const g = i >= n - fade ? (n - i) / fade : 1;
    s[i] = g * amp * Math.sin(2 * Math.PI * hz * (t0 + i / RATE));
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

bro.sense.start({});
assert(bro.sense.isActive(), "sense active after start");
assert(bro.sense.sampleRate() === RATE, "hub at 16 kHz");

// 0.5 s silence, then 1 s 1 kHz tone at -20 dB, with a click at 0.25 s.
const pcm = concat(silence(0.5), tone(1.0, 1000, 0.1, 0.5));
for (let i = 0; i < 16; i++) pcm[Math.floor(0.25 * RATE) + i] = 0.8 * (((i * 2654435761) >>> 16 & 0xffff) / 32768 - 1);

const s = bro.sense.feed(pcm);
assert(s !== undefined && s !== null, "headless feed returns a snapshot");
assert(s.frames > 140, "frames advanced (" + s.frames + ")");
assert(s.voice, "voice active during the tone");
assert(s.tonal, "tonal during the tone");
assert(s.periodicity > 0.9, "high periodicity (" + s.periodicity.toFixed(3) + ")");
assert(Math.abs(s.dominantHz - 1000) < 70, "dominantHz near 1 kHz (" + s.dominantHz.toFixed(0) + ")");
assert(s.onsets >= 1, "the click registered as an onset (" + s.onsets + ")");
assert(s.voiceEvents >= 1, "voice transition counted");

// Counters are monotonic across feeds.
const s2 = bro.sense.feed(silence(0.5));
assert(s2.frames > s.frames, "frames keep advancing");
assert(s2.onsets === s.onsets, "no new onsets in silence");

bro.sense.stop();
assert(!bro.sense.isActive(), "inactive after stop");
console.log("sense_smoke: all assertions passed");
