/**
 * bro.listen — the shared listening stream's own surface
 *
 * The listening stack (bro.kws, bro.sense, bro.wake, bro.gesture) all ride ONE
 * shared front-end: a single raw 16 kHz tap + PCEN mel pass + per-model forward,
 * with the tenants attached as members (see the listen host design). bro.listen
 * is that shared stream's OWN surface — things about the stream itself, not any
 * one tenant.
 *
 * Today it exposes opt-in raw-audio RETENTION: a ring of the recent stream you
 * can replay or scrub by frame range — e.g. to hear exactly what a bro.kws spot
 * or a bro.gesture match fired on. It is SOURCE-AGNOSTIC: it captures whatever
 * drives the host — the live mic, a scripted feed(), and any future non-mic
 * source (system-audio loopback) wired into the stack — so replay works for
 * every input the stack listens to, not just the mic.
 *
 * Off by default (no memory cost until enabled). Retention is single-producer
 * (the feed thread) with a lock-free reader; a frame is only overwritten after a
 * full retention period, so reads of recent audio never tear.
 *
 * Frame axis: total samples consumed / hop — the SAME axis bro.sense.snapshot()
 * .frames reports (sense is the host's first/always member). A fresh stream
 * (first start after a full teardown) restarts the axis at 0, like the tenants.
 */

// ── Retention ─────────────────────────────────────────────────────────────────

/**
 * Enable / resize raw-audio retention to `seconds` of the shared stream. 0
 * disables it and frees the buffer. ~64 KB per second (16 kHz f32), so 10 min ≈
 * 38 MB. Takes effect immediately when the stream is live, else on the next
 * start. Safe to call repeatedly (re-sizes).
 *
 * @param {number} seconds  retention depth in seconds (0 = off).
 */
bro.listen.retain(600);   // keep the last 10 minutes

/**
 * The retained raw PCM for an inclusive frame range, or null when retention is
 * off or the range fell outside the held window (too old / in the future).
 * Mono Float32Array at bro.listen.info().rate. The range is on the stream frame
 * axis — pass the frames a tenant reported (e.g. an onSpot span, or
 * bro.sense.snapshot().frames), no conversion needed.
 *
 * @param {number} startFrame  inclusive
 * @param {number} endFrame    inclusive
 * @returns {Float32Array|null}
 */
const clip = bro.listen.audio(startFrame, endFrame);
// e.g. play it back:
//   const ctx = new AudioContext();
//   const buf = ctx.createBuffer(1, clip.length, bro.listen.info().rate);
//   buf.getChannelData(0).set(clip);
//   const src = ctx.createBufferSource(); src.buffer = buf;
//   src.connect(ctx.destination); src.start();

/** @returns {number} the current stream frame (total samples consumed / hop). */
const now = bro.listen.frame();

/**
 * Retention status — for a UI / scrubber.
 * @returns {{
 *   active: boolean,        // retention enabled
 *   seconds: number,        // configured depth
 *   rate: number,           // PCM sample rate (16000)
 *   hop: number,            // samples per frame (frame -> sample factor)
 *   frameRate: number,      // rate / hop (frames per second)
 *   streamFrame: number,    // current stream frame
 *   heldFrames: number,     // frames currently available to read back
 *   heldSeconds: number,    // ... in seconds
 * }}
 */
const info = bro.listen.info();
