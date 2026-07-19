/**
 * bro.time — global pause + timescale
 *
 * The Godot Engine.time_scale / SceneTree.paused analog. The engine owns one
 * scaled clock, advanced each frame by wallDt * scale (0 while paused), and
 * everything gameplay-visible runs on it:
 *
 *   SCALED (obeys bro.time):
 *     - setTimeout / setInterval deadlines
 *     - requestAnimationFrame timestamps — and while paused, rAF callbacks
 *       are skipped entirely (the web's _process analog); timescale changes
 *       only the timestamp a callback receives, never the firing cadence
 *     - performance.now()
 *     - CSS transitions and animations
 *     - physics stepping (fixed timestep preserved — pause/timescale change
 *       how much sim time accumulates per wall second, so scale 2 runs the
 *       sim at double speed with identical determinism)
 *     - scene-graph animations and AI agents (syncAgents/tickAnimations dt)
 *     - <iframe> sub-documents (timers + rAF; frozen entirely while paused).
 *       NOTE: the sub-document is governed by bro.time but cannot see it —
 *       `bro.time` is installed on the primary app context only, so code
 *       running inside an <iframe> or a secondary window (bro.window.open)
 *       has no bro.time at all and can neither read nor change the clock.
 *       Drive it from the host realm and message the result in.
 *
 *   WALL CLOCK (exempt — the shell never freezes):
 *     - system panels: menu bar, perf HUD, settings overlay, splash
 *     - Date.now() (real time, as on the web)
 *     - Worker threads (own event loops)
 *     - wheel-scroll smoothing (scrolling still eases while paused)
 *     - GC cadence, UI render throttle, frame pacing
 *     - network/steam service pumps
 *
 *   AUDIO:
 *     - paused = true suspends audio output (broaudio master pause — a
 *       transport freeze: voices, clips, and scheduled events resume exactly
 *       in place on unpause, nothing is dropped). With no audio engine
 *       running (headless, or a build/device with no audio output) this half
 *       of the pause contract is simply a no-op — everything else about
 *       pause still applies.
 *     - timescale does NOT touch audio: playback always renders at real
 *       rate, so there is never a pitch shift
 *
 * Headless: advanceTime(ms) advances the scaled clock by ms * bro.time.scale
 * — and not at all while paused — while virtual time (system panels, splash,
 * audio pump, brokit ticks) advances by the full ms. This makes pause and
 * timescale fully testable headlessly.
 */

// ── Properties ───────────────────────────────────────────────────────────────

bro.time.scale;          // number — time multiplier, default 1
bro.time.scale = 0.5;    // slow motion (half speed)
bro.time.scale = 2;      // fast-forward (double speed)
bro.time.scale = 0;      // freeze gameplay time without the pause side-effects
                         // (audio keeps playing, rAF keeps firing)
// Assignments clamp to [0, 100]; non-finite values are ignored. The value is
// first put through ToNumber, so an unparseable string ('fast') becomes NaN
// and is ignored, but a BigInt or Symbol THROWS TypeError rather than being
// ignored — validate before assigning if the value comes from user data.

bro.time.paused;         // boolean — global pause, default false
bro.time.paused = true;  // effective scale 0 + rAF skipped + audio suspended
bro.time.paused = false; // everything resumes exactly where it stopped

bro.time.now;            // number — current scaled engine time in ms
                         // (read-only; the clock timers/rAF/transitions run on)

// ── Pause menu idiom ─────────────────────────────────────────────────────────
//
// Gameplay (rAF loops, timers, physics, CSS animations) freezes; DOM input
// events still dispatch, so a pause overlay built from ordinary elements
// keeps working. Drive its "animations" from input, or leave them to the
// engine-level system panels which stay on wall time.

window.addEventListener('action', (e) => {
    if (e.name === 'pause') bro.time.paused = !bro.time.paused;
});

// ── Slow-motion effect ───────────────────────────────────────────────────────

bro.time.scale = 0.25;                       // bullet time
setTimeout(() => { bro.time.scale = 1; }, 500);  // NOTE: this timeout is itself
// scaled — 500 scaled ms = 2000 wall ms at scale 0.25. Time your effect in
// wall clock with Date.now() if you want a fixed real-world duration.

// ── Headless testing ─────────────────────────────────────────────────────────

bro.time.scale = 0.5;
advanceTime(100);        // scaled clock advances 50ms; timers/rAF/physics see 50ms
bro.time.paused = true;
advanceTime(1000);       // gameplay time does not move at all
