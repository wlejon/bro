// bro.sense — tier-0 acoustic sensor bus (brosoundml::SensorHub)
// ================================================================
//
// The fast, cheap, always-on layer of the listening stack. One streaming
// PCEN mel front-end (the same recipe bro.wake / bro.kws run) drives a set
// of per-frame DSP sensors — no model, no enrollment, no GPU — and publishes
// every signal into one lock-free snapshot you poll:
//
//   level     window RMS / peak / dBFS on raw PCM (no AGC — this is the
//             stack's one absolute-loudness signal).
//   voice     energy VAD: adaptive noise floor + SNR gate + hangover.
//   onset     PCEN spectral flux: percussive transients (clicks, taps,
//             snaps) fire here in a single 10 ms frame — including the
//             isolated one-shot sounds a sequence matcher (bro.kws)
//             structurally cannot hold onto.
//   tonality  autocorrelation periodicity + dominant frequency: sustained
//             periodic sounds (whistles, hums — pure or harmonic) read as a
//             high-periodicity run with a stable dominantHz.
//
// bro.sense is designed for FUSION, not for being right on its own: each
// sensor catches what it can, within a frame of it happening, and an app (or
// a heavier tenant like bro.kws / bro.wake / streaming STT) combines the
// signals into a conclusion. Latency is one mel frame (10 ms) + your poll
// interval.
//
// Plumbing: bro.sense is a member of the engine's SHARED listen host — one
// raw (no-AGC) mic tap + ring + inference task drive a single PCEN front-end
// feeding every attached tenant, so running bro.sense alongside bro.kws costs
// one feature pass and both hear the SAME stream.
//
// There are no callbacks. Every momentary boolean (onset, voice, tonal) is
// paired with a monotonic counter (onsets, voiceEvents, tonalEvents) and a
// last-event frame index, so polling at frame rate — or slower — still
// observes every event as a counter delta. Diff the counters between polls.

// ─── Lifecycle ──────────────────────────────────────────────────────────────

// Start the sensor bus on the live mic. All options are optional; the
// defaults are sensible for a normal room.
bro.sense.start({
  // voice (energy VAD)
  vadFloorDb:    -55,   // below this a frame can never be voice (dBFS)
  vadSnrDb:        8,   // frame must beat the adaptive noise floor by this
  vadRiseDbps:     6,   // noise-floor release rate, dB/s (attack is instant)
  vadHangFrames:  25,   // hold `voice` this long past the last hot frame

  // onset (spectral flux)
  onsetRatio:           2.5,   // flux must exceed its slow EMA by this factor
  onsetAbs:             0.05,  // ... and this absolute floor
  onsetEma:             0.05,  // EMA coefficient (~0.5 s time constant)
  onsetRefractoryFrames: 5,    // at most one onset per 50 ms

  // tonality (autocorrelation periodicity)
  tonalMinPeriodicity: 0.6,    // [0,1]: tones/hums > 0.9, white noise ~0.15
  tonalFminHz:          80,    // pitch search range: low hum ...
  tonalFmaxHz:        4000,    // ... to sharp whistle
});

bro.sense.isActive();    // -> bool
bro.sense.stop();        // detach the mic tap + inference task, drop the hub
bro.sense.sampleRate();  // -> 16000 (the hub's PCM rate)
bro.sense.stats();       // -> { framesDelivered, samplesDelivered, rollingPeak }
                         //    (mic-tap diagnostics; null when inactive)

// ─── Polling ────────────────────────────────────────────────────────────────

// Lock-free read of the latest coherent sensor frame (all values taken on
// the same mel frame). Returns null when nothing has been started.
const s = bro.sense.snapshot();
// {
//   frames: 1234,          // mel frames processed since start (10 ms each)
//   t: 12.34,              // stream time (s) at the end of the latest frame
//
//   // level
//   rms: 0.031, peak: 0.18, db: -30.2,
//
//   // voice
//   voice: true,
//   noiseFloorDb: -62.1,   // current adaptive floor
//   snrDb: 31.9,           // db - noiseFloorDb
//   voiceFrames: 87,       // consecutive frames voice has been true
//   voiceEvents: 3,        // silence->voice transitions since start
//   lastVoiceFrame: 1147,
//
//   // onset
//   flux: 0.012,           // this frame's positive PCEN flux
//   onset: false,          // true ONLY on the triggering frame — diff
//   onsets: 17,            //   `onsets` between polls instead
//   lastOnsetFrame: 1201,
//
//   // tonality
//   periodicity: 0.97,     // normalized autocorrelation peak [0,1]
//   dominantHz: 1378,      // frequency of the winning period (meaningful
//                          //   when periodicity is high)
//   tonal: true,
//   tonalFrames: 42,       // consecutive tonal frames (a sustained whistle
//                          //   reads as a long run with stable dominantHz)
//   tonalEvents: 5,
//   lastTonalFrame: 1233,
// }

// ─── Typical fusion loop ────────────────────────────────────────────────────

let last = bro.sense.snapshot();
setInterval(() => {
  const s = bro.sense.snapshot();
  if (!s) return;

  // Every onset since the last poll is visible as a counter delta:
  if (s.onsets > last.onsets) {
    console.log(`${s.onsets - last.onsets} transient(s), last @ frame ${s.lastOnsetFrame}`);
  }

  // A whistle: sustained tonal run at a stable high frequency.
  if (s.tonal && s.tonalFrames > 30 && s.dominantHz > 800) {
    console.log(`whistling at ~${Math.round(s.dominantHz)} Hz`);
  }

  // Gate heavier work on cheap evidence (e.g. resume bro.kws on voice).
  if (s.voice && !last.voice) {
    /* someone started making sound `s.t` seconds in */
  }

  // Fuse with tier-2 partial evidence: bro.kws.progress() is the same kind of
  // lock-free poll, so one loop reads "voice is live AND a phrase is mostly
  // matched" before any onSpot fires.
  const prog = bro.kws.progress();
  if (s.voice && prog && prog.templates.some(t => t.progress > 0.6)) {
    /* arm a heavier confirmation tier (e.g. streaming STT) */
  }
  last = s;
}, 50);

// ─── Headless / scripted feeding ────────────────────────────────────────────

// In headless mode (no inference worker) feed() runs the shared bus
// synchronously and returns the post-feed snapshot — deterministic for
// tests. The listen host carries ONE stream, so the feed advances every
// attached tenant (audio fed here also drives bro.kws, and vice versa).
// While an inference worker is running, feed() writes the live shared ring
// instead (returns undefined) and you poll snapshot() as usual. Refused
// while the real mic is capturing.
const pcm = new Float32Array(16000);   // 1 s of synthetic 16 kHz audio
const snap = bro.sense.feed(pcm);

// ─── Offline clip analysis ──────────────────────────────────────────────────
//
// bro.sense.analyze(samples, opts?) runs a PRIVATE SensorHub over a clip with
// no effect on the live bus (callable any time, even before start()), returning
// the per-frame sensor timeline as columnar typed arrays. It is the SAME stream
// bro.gesture.enrollFromAudio sees, so a tool can map each frame to its samples
// and overlay onsets / tonal runs / pitch onto a waveform — e.g. to show why a
// clip enrolled the way it did, or why a cough's wandering pitch reads tonal but
// unsteady. `opts` overlays the same sensor-policy keys as start().
//
//   const a = bro.sense.analyze(clip);   // clip: Float32Array @ sampleRate()
//   // a = {
//   //   frames: 142,            // number of sensor frames
//   //   hop: 160, win: 400,     // frame f covers samples [f*hop, f*hop+win)
//   //   rate: 16000,
//   //   frameMs: 10,
//   //   db:          Float32Array(frames),   // per-frame level
//   //   dominantHz:  Float32Array(frames),   // per-frame pitch (sub-lag refined)
//   //   periodicity: Float32Array(frames),   // per-frame autocorrelation peak
//   //   flags:       Int32Array(frames),     // bit0 voice, bit1 tonal, bit2 onset
//   // }
//   for (let f = 0; f < a.frames; f++) {
//     const onset = (a.flags[f] & 4) !== 0;
//     const tonal = (a.flags[f] & 2) !== 0;
//     if (tonal) markPitch(f * a.hop, a.dominantHz[f]);
//   }
