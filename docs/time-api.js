/**
 * =============================================================================
 * bro.time — global pause + timescale
 * =============================================================================
 *
 * The Godot Engine.time_scale / SceneTree.paused analog. The engine owns one
 * scaled clock, advanced each frame by wallDt * scale (0 while paused), and
 * everything gameplay-visible runs on it:
 *
 *   SCALED (obeys bro.time):
 *     - setTimeout / setInterval deadlines
 *     - requestAnimationFrame timestamps, and while paused, rAF callbacks
 *       are skipped entirely (the web's _process analog); timescale changes
 *       only the timestamp a callback receives, never the firing cadence
 *     - performance.now() — advances continuously within a frame in windowed
 *       and server runs (interpolated from wall time at the scale in effect,
 *       so it freezes exactly while paused); in headless it is the virtual
 *       clock and holds still between advanceTime() calls
 *     - CSS transitions and animations
 *     - physics stepping (fixed timestep preserved: pause/timescale change
 *       how much sim time accumulates per wall second, so scale 2 runs the
 *       sim at double speed with identical determinism)
 *     - scene-graph animations and AI agents (syncAgents/tickAnimations dt)
 *     - <iframe> sub-documents (timers + rAF; frozen entirely while paused).
 *       NOTE: the sub-document is governed by bro.time but cannot see it,
 *       `bro.time` is installed on the primary app context only, so code
 *       running inside an <iframe> or a secondary window (bro.window.open)
 *       has no bro.time at all and can neither read nor change the clock.
 *       Drive it from the host realm and message the result in.
 *
 *   WALL CLOCK (exempt: the shell never freezes):
 *     - system panels: menu bar, perf HUD, settings overlay, splash
 *     - Date.now() (real time, as on the web)
 *     - Worker threads (own event loops)
 *     - wheel-scroll smoothing (scrolling still eases while paused)
 *     - GC cadence, UI render throttle, frame pacing
 *     - network/steam service pumps
 *
 *   AUDIO:
 *     - paused = true suspends audio output (broaudio master pause, a
 *       transport freeze: voices, clips, and scheduled events resume exactly
 *       in place on unpause, nothing is dropped). With no audio engine
 *       running (headless, or a build/device with no audio output) this half
 *       of the pause contract is simply a no-op: everything else about
 *       pause still applies.
 *     - timescale does NOT touch audio: playback always renders at real
 *       rate, so there is never a pitch shift
 *
 * Headless: advanceTime(ms) advances the scaled clock by ms * bro.time.scale,
 * and not at all while paused, while virtual time (system panels, splash,
 * audio pump, brokit ticks) advances by the full ms. This makes pause and
 * timescale fully testable headlessly.
 *
 * @example
 *   // Pause menu idiom
 *   window.addEventListener('action', (e) => {
 *     if (e.name === 'pause') bro.time.paused = !bro.time.paused;
 *   });
 *
 * @example
 *   // Slow-motion effect
 *   bro.time.scale = 0.25;                           // bullet time
 *   setTimeout(() => { bro.time.scale = 1; }, 500);  // 500 scaled ms = 2000 wall ms
 *
 * @example
 *   // Headless testing
 *   bro.time.scale = 0.5;
 *   advanceTime(100);        // scaled clock advances 50ms; timers/rAF/physics see 50ms
 *   bro.time.paused = true;
 *   advanceTime(1000);       // gameplay time does not move at all
 */

// ── Namespaces ───────────────────────────────────────────────────────────────

/**
 * Global engine time and timescale control namespace.
 */
/**
 * Time multiplier (default 1.0). Clamps to [0, 100]; non-finite values are ignored.
 * 0.5 = slow motion, 2.0 = fast forward, 0 = freeze gameplay time without pause side-effects.
 * @type {number}
 */
bro.time.scale;

/**
 * Global pause state (default false).
 * true = effective scale 0 + rAF callbacks skipped + audio output suspended.
 * false = everything resumes exactly where it stopped.
 * @type {boolean}
 */
bro.time.paused;

/**
 * Current scaled engine time in milliseconds (read-only).
 * This is the clock timers, rAF, transitions, and physics run on.
 * @readonly
 * @type {number}
 */
bro.time.now;

