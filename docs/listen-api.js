/**
 * bro.listen — open N concurrent, unmixed listening streams
 *
 * A listening STREAM is one independent pipeline: a single audio SOURCE (the
 * microphone, the whole-system render mix, or one application's audio) → one raw
 * 16 kHz ring → one PCEN mel front-end → up to one each of {sense, kws, wake,
 * gesture} attached as members. One feature pass, one forward per attached
 * model, many listeners — all hearing THAT source.
 *
 * Multiple streams run concurrently and independently, with NO mixing: a mic
 * stream driving wake/kws for voice commands can run alongside a system-audio
 * stream driving streaming STT for whatever the machine is playing. Each is its
 * own source, ring, front-end, retention and tenant set. Spinning up one stream
 * per channel is how you listen to L/R (or per-app) separately.
 *
 *   bro.listen.open(source?)  → a ListenStream handle (see below)
 *   bro.listen.supported()    → is render-side (system / per-app) capture
 *                               available on this build/OS?
 *   bro.listen.apps()         → [{pid, name}] apps holding a render session
 *
 * THE DEFAULT MIC. The globals bro.kws / bro.wake / bro.sense / bro.gesture (and
 * bro.listen.retain/audio/frame/info) target one shared, implicit
 * default-microphone stream — the ergonomic path when you only need the mic.
 * stream.kws / .wake / .sense / .gesture on a handle target THAT stream. The two
 * are the same implementation, just homed to different streams.
 *
 * WEIGHTS LOAD ONCE. bro.kws.load() / bro.wake.load() read a model into a shared,
 * read-only net; each stream that listens builds its own lightweight session
 * over it (its own templates, threshold, matcher state). The same vocabulary /
 * wake word runs on N streams without copying weights. sense and gesture are
 * model-free — each stream just gets its own SensorHub / GestureSpotter.
 *
 * Frame axis: total samples consumed / hop — the SAME axis a stream's
 * sense.snapshot().frames and kws onSpot spans report. Each stream has its own
 * axis, restarting at 0 when (re)opened.
 */

// ── Opening streams ─────────────────────────────────────────────────────────

/**
 * Open an independent listening stream. Loopback sources start capturing
 * immediately (audio flows / can be retained before any model is attached).
 * Throws if the source is unavailable (loopback unsupported on this build/OS, or
 * the target process is gone).
 *
 * @param {('mic'|'system'|{mic?:boolean,system?:boolean,process?:number,pid?:number,exclude?:boolean,channel?:number})} [source]
 *   - undefined | 'mic' | {mic:true}        → the microphone
 *   - 'system' | {system:true}              → the whole-system render mix (loopback)
 *   - {process: pid} | {pid, exclude:true}  → one app's audio (or all-except-it)
 *   - any source may carry { channel: n }   → pick one channel (-1 = downmix all)
 * @returns {ListenStream}
 */
const mic    = bro.listen.open('mic');         // a second, independent mic stream
const system = bro.listen.open('system');      // whole-system loopback
const app    = bro.listen.open({ process: 12345 });  // one app by pid

/** @returns {boolean} is render-side (system / per-app) capture available? */
if (bro.listen.supported()) { /* system / process streams can be opened */ }

/** @returns {Array<{pid:number,name:string}>} apps currently playing audio. */
for (const { pid, name } of bro.listen.apps()) console.log(pid, name);

// ── The flagship: transcribe system audio while taking mic commands ───────────
//
// Two unmixed streams at once — no model weights duplicated for the mic side,
// and the system side feeds STT straight from retained PCM.

const sys = bro.listen.open('system');
sys.retain(30);                                  // hold 30 s of system audio
// … later, pull a window and transcribe it (bro.stt loads its own model):
//   const pcm = sys.audio(startFrame, endFrame);
//   const text = await bro.stt.transcribe(whisper, pcm);
// Meanwhile, mic voice-commands run on the default-mic globals, concurrently:
bro.kws.load({ weights: '.../english.bpm' });
bro.kws.enroll('next-slide', bro.tts.phonemize('next slide'));
bro.kws.listen({ onSpot: (name) => doCommand(name) });   // default mic

// ── ListenStream handle ───────────────────────────────────────────────────────
//
// Returned by bro.listen.open(). Holds the stream; closing it (explicitly or by
// GC) frees the source + front-end. Stream ids are monotonic (never reused), so
// a stale handle can never address a different stream — its methods no-op once
// closed.

/** @typedef {object} ListenStream */

mic.id;        // number — the stream id (monotonic)
mic.kind;      // 'mic' | 'system' | 'process'
mic.valid;     // boolean — false once closed

// Per-stream raw-audio retention (same shape as the bro.listen.* globals, which
// are just these scoped to the default mic).
mic.retain(600);                          // keep the last 10 min of THIS stream
const clip = mic.audio(startFrame, endFrame);   // Float32Array | null
const f    = mic.frame();                 // current stream frame
const info = mic.info();                  // { active, seconds, rate, hop,
                                          //   frameRate, streamFrame, heldFrames,
                                          //   heldSeconds }

// Scripted/headless feed at the stream's rate (live capture writes the ring
// itself; this is for tests / offline pumping).
mic.feed(new Float32Array(160));

mic.close();   // detach members, stop the source, free infra. Idempotent.

// ── Per-stream model views ────────────────────────────────────────────────────
//
// A handle exposes the listening models scoped to ITS stream. Same surfaces as
// the bro.kws / bro.wake / bro.sense / bro.gesture globals (see their api docs),
// just bound to this stream instead of the default mic.

// Wake-word on system audio, kws on the mic — at the same time, one net each:
bro.wake.load({ weights: '.../computer.bw' });    // shared net, loaded once
system.wake.listen({ onFire: () => console.log('heard it in the system audio') });
bro.wake.listen({ onFire: () => console.log('heard it on the mic') });   // default mic

// tier-0 sensing + gesture on a specific app's audio:
app.sense.start();
app.gesture.enrollFromAudio('jingle', referenceClip);
app.gesture.listen({ onGesture: (name) => console.log('matched', name) });

system.kws;      // → the bro.kws surface, scoped to the system stream
system.wake;     // → bro.wake,    scoped to this stream
system.sense;    // → bro.sense,   scoped to this stream
system.gesture;  // → bro.gesture, scoped to this stream

// ── Retention on the default mic (globals) ────────────────────────────────────
//
// bro.listen.retain/audio/frame/info are the per-stream retention API scoped to
// the shared default-microphone stream — the same one bro.kws/.wake/.sense/
// .gesture target. Use them when you only care about the mic.

bro.listen.retain(600);
const micClip = bro.listen.audio(startFrame, endFrame);
const micNow  = bro.listen.frame();
const micInfo = bro.listen.info();
