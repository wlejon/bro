/**
 * element.animate(), Web Animations API, CSS animations as CSSAnimation and
 * CSS transitions as CSSTransition
 *
 * One animation model: a CSS @keyframes animation IS a Web Animation (a
 * CSSAnimation, per CSS Animations 2), and so is a running CSS transition (a
 * CSSTransition, per CSS Transitions 2), listed by getAnimations() and driven
 * by the same engine records as element.animate(). Interpolated values are
 * injected into computed style during style resolution (CSS transitions
 * first, then CSS animations in animation-name order, then script animations
 * in creation order; later wins), the clock is the engine's scaled bro.time
 * clock (bro.time.paused freezes them, bro.time.scale stretches them),
 * transform/opacity-only animations get compositor-layer promotion, and
 * headless advanceTime(ms) drives them deterministically.
 *
 * PROPERTY COVERAGE: inherited from the transition interpolator:
 *   - numbers and lengths (opacity, width, top, margin-*, border-radius, …
 *     any "12px"/"0.5"-shaped value; the unit comes from the target)
 *   - colors (#hex, rgb(), rgba())
 *   - CSS function lists with matching shapes (transform, filter, e.g.
 *     'rotate(0deg)' → 'rotate(360deg)', 'scale(1) translateX(0px)' →
 *     'scale(2) translateX(40px)')
 *   Anything else is non-interpolable and snaps at 50% (discrete-ish).
 *   Values are not validated: they land in computed style verbatim.
 *
 * EASING: every <easing-function>, the same parser for WAAPI options,
 *   keyframe easings, and CSS transition/animation timing functions:
 *   linear | ease | ease-in | ease-out | ease-in-out | cubic-bezier(...) |
 *   step-start | step-end | steps(n[, jump-start|jump-end|jump-none|
 *   jump-both|start|end]) | linear(<number> [<percentage>{1,2}], ...).
 *   Anything else throws a TypeError (element.animate, updateTiming). The
 *   default is linear (the WAAPI default). getTiming().easing reads back the
 *   canonical form ('steps(4, end)' reads 'steps(4)').
 *
 * DELIBERATE SIMPLIFICATIONS (vs the full spec):
 *   - Composite modes other than "replace" are ignored.
 *   - `pending` is always false (play/pause apply immediately), so `ready`
 *     is an already-resolved promise and updatePlaybackRate() applies at
 *     once, like setting playbackRate. `effect` cannot be swapped and has no
 *     setKeyframes; `timeline` is always document.timeline; `new
 *     KeyframeEffect(...)` is not constructible; iterationStart is always 0.
 *   - Object-form keyframes distribute values evenly; an explicit `offset`
 *     list inside the object form is ignored (use the array form for
 *     explicit offsets). An `easing` array is applied cyclically across the
 *     merged keyframes.
 *   - reverse() on an infinite animation seeks to 0 (spec throws).
 *   - Transitions start for any computed value that changes under a matching
 *     transition-property, discrete ones included (they snap at 50%);
 *     transition-behavior is not consulted.
 *
 * LIFETIME:
 *   - The Animation object holds an id into an engine-side record, never a
 *     raw element pointer; records resolve their element generation-checked,
 *     so removing or destroying the element (or tearing the document down)
 *     mid-animation is always safe.
 *   - An animation on a removed-but-alive element keeps ticking and will
 *     fire finished normally (matching CSS transitions); it just stops
 *     rendering.
 *   - Running/paused animations keep their Animation object alive even if
 *     script drops every reference (a finish can still be delivered, same
 *     as browsers). A finished forwards-filling animation keeps applying its
 *     final value even after the object is GC'd.
 */

// ── Starting an animation ────────────────────────────────────────────────────

const el = document.querySelector('#box');

// Array-of-keyframes form. camelCase property names; offset/easing optional.
const anim = el.animate([
    { opacity: 0, transform: 'translateX(0px)' },
    { opacity: 1, transform: 'translateX(100px)', offset: 0.8, easing: 'ease-out' },
    { opacity: 0.5, transform: 'translateX(80px)' }
], {
    duration: 1000,          // ms per iteration (required for visible motion)
    delay: 0,                // ms before the first iteration
    endDelay: 0,             // ms appended after the last iteration
    iterations: 1,           // number, Infinity ok
    direction: 'normal',     // normal | reverse | alternate | alternate-reverse
    easing: 'linear',        // whole-iteration easing (default linear)
    fill: 'none',            // none | forwards | backwards | both
    id: 'slide-in'           // optional label, exposed as anim.id
});

// Number shorthand: options = duration in ms.
el.animate([{ opacity: 0 }, { opacity: 1 }], 300);

// Object-of-arrays form, values distribute evenly across the duration.
el.animate({ opacity: [0, 1], transform: ['scale(0.5)', 'scale(1)'] }, 400);

// Single keyframe animates from the element's current (base) value.
el.animate([{ opacity: 0.2 }], { duration: 300, fill: 'forwards' });

// Missing offsets are auto-distributed (first → 0, last → 1, interior spaced
// evenly). Non-monotonic or out-of-range offsets throw a TypeError.

// ── The Animation object ─────────────────────────────────────────────────────

anim.play();             // start / resume / restart-after-finish (auto-rewinds)
anim.pause();            // freeze at the current time
anim.cancel();           // stop, drop all effect output → playState 'idle';
                         // rejects `finished` with an AbortError DOMException
                         // and replaces it with a fresh pending promise.
                         // play() afterwards restarts from scratch.
anim.finish();           // jump to the end (start when playbackRate < 0);
                         // fill:forwards keeps the final value applied.
                         // Throws InvalidStateError on infinite animations.
anim.reverse();          // flip playbackRate and play (from the end if done)
anim.updatePlaybackRate(2); // change the rate without a jump in currentTime
anim.commitStyles();     // write the effect's current values into the target's
                         // inline style (keep an end state, then cancel())
anim.persist();          // exempt a finished fill-forwards animation from
                         // automatic removal (see "Replaced animations")

anim.currentTime;        // number ms (null when idle), get/set to seek
anim.currentTime = 500;  // seek; un-finishes a finished animation
anim.playbackRate;       // get/set; 0 freezes, negative runs backwards
anim.playState;          // 'idle' | 'running' | 'paused' | 'finished'
anim.pending;            // always false (control ops apply immediately)
anim.id;                 // the options.id string (get/set)

// Promise resolved with the animation when it finishes; rejected with an
// AbortError-shaped DOMException when it is canceled. Created lazily on
// first access; replaced with a fresh pending promise after cancel and when
// a finished animation is played/seeked back into the running state.
await anim.finished;

anim.onfinish = (e) => { /* e.type === 'finish', e.currentTime, e.target */ };
anim.oncancel = (e) => { /* e.type === 'cancel' */ };
anim.onremove = (e) => { /* e.type === 'remove': replaced, see below */ };
anim.replaceState;       // 'active' | 'removed' | 'persisted'

// Already resolved with the animation (nothing is ever pending).
await anim.ready;

// ── Timeline & start time ────────────────────────────────────────────────────

// The DocumentTimeline (an AnimationTimeline) every element.animate() runs on.
// currentTime is the engine's scaled bro.time clock in ms, so it pauses and
// stretches with bro.time and advances with headless advanceTime(ms).
document.timeline.currentTime;
anim.timeline === document.timeline;   // true

// Timeline time at which currentTime was 0; null while paused, finished via
// a hold, or idle. For a running animation:
//   anim.currentTime === (document.timeline.currentTime - anim.startTime) * anim.playbackRate
anim.startTime;
anim.startTime = document.timeline.currentTime - 250;   // seek to 250 ms and run

// ── The effect ───────────────────────────────────────────────────────────────

// A KeyframeEffect (an AnimationEffect); the same object on every read.
const fx = anim.effect;
fx.target;               // the animated element (null once it is gone)
fx.getTiming();          // { delay, endDelay, fill, iterationStart: 0, iterations,
                         //   duration, direction, easing } as given
fx.getComputedTiming();  // getTiming() plus { activeDuration, endTime, localTime,
                         //   progress, currentIteration }; progress is directed
                         //   and eased, null outside the active interval when
                         //   no fill applies there
fx.getKeyframes();       // [{ offset, computedOffset, easing, composite,
                         //    <camelCase property>: '<value>', ... }, ...]
fx.updateTiming({ duration: 2000, easing: 'steps(4)' });
                         // any EffectTiming members; a finished animation whose
                         // end moves past its current time runs again. Invalid
                         // values throw a TypeError.

// ── Replaced animations ──────────────────────────────────────────────────────
//
// A finished fill:'forwards' script animation whose every property a later
// finished fill-forwards animation on the same element also animates is
// removed (Web Animations §5.5): replaceState becomes 'removed', onremove
// fires, it stops applying and leaves getAnimations(). This is what keeps
// fire-and-forget `fill: 'forwards'` animations from piling up. persist()
// opts one out. CSS animations are never removed this way.

// ── CSS animations (CSSAnimation) ────────────────────────────────────────────
//
// Every layer of an element's animation-name list (so `animation: a 1s, b 2s`
// runs both) is a CSSAnimation, an Animation with one extra member:
const cssAnim = el.getAnimations().find((a) => a instanceof CSSAnimation);
cssAnim.animationName;     // the @keyframes name
// Its timing comes from the animation-* longhands (getTiming().easing is
// 'linear': animation-timing-function eases each keyframe interval and is
// reported per keyframe by getKeyframes()). It follows the markup: changing
// animation-duration re-times it, animation-play-state pauses it, removing
// the name cancels it. Script can drive it like any animation:
cssAnim.pause(); cssAnim.currentTime = 500; cssAnim.play();
// - play()/pause() take playback over: animation-play-state no longer applies.
// - effect.updateTiming() members stop following their longhands.
// - cancel() stops it; it restarts only if animation-name changes and back.
// - animationstart / animationiteration / animationend / animationcancel
//   fire from its phase, so a script seek or finish() fires them too
//   (animationstart after the delay, not when the name is set).
// - display:none on the element or an ancestor cancels it (animationcancel;
//   it leaves getAnimations()); displayed again, a new CSSAnimation starts
//   from the beginning.

// ── CSS transitions (CSSTransition) ──────────────────────────────────────────
//
// A running transition is a CSSTransition, an Animation with one extra member:
el.style.opacity = '0';    // under `transition: opacity 1s`
const tr = el.getAnimations().find((a) => a instanceof CSSTransition);
tr.transitionProperty;     // 'opacity' (the longhand, also under `all`)
// Two keyframes, from the value before the change to the value after; its
// duration and delay come from transition-duration / -delay, and the
// transition-timing-function is the first keyframe's easing (getTiming()
// .easing is 'linear'). The start value holds through the delay.
// - Script can pause, seek, finish() or cancel() it; either of the last two
//   leaves the element on the end value, with no new transition.
// - Changing the value mid-flight cancels it and starts a new CSSTransition
//   from where it had got to; going back to where it came from is shortened
//   to the time it had run (CSS Transitions §3.1 reversing).
// - It is cancelled when transition-property stops naming the property, and
//   by display:none on the element or an ancestor; no transition starts on an
//   element that is, or was just, display:none.
// - transitionrun (at once) / transitionstart (after the delay) /
//   transitionend / transitioncancel fire from its phase, so a script seek,
//   finish() or cancel() fires them too.
// - Done, it leaves getAnimations() (a transition does not fill forwards).

// ── Enumeration ──────────────────────────────────────────────────────────────

el.getAnimations();        // Animation[] for this element (running/paused +
                           // finished-while-filling-forwards) in composite
                           // order: CSS transitions, then CSS animations in
                           // animation-name order, then script ones in
                           // creation order; identity-preserving (same
                           // objects you got back). Styles are brought up to
                           // date first, so an animation or transition a
                           // class change just started is listed.
document.getAnimations();  // the whole document: CSS transitions, then CSS
                           // animations, each by the tree order of their
                           // elements, then script animations by creation

// ── Interplay ────────────────────────────────────────────────────────────────
//
// - Overrides inline style and the cascade while active, and sits above CSS
//   transitions AND CSS animations for the properties it animates. A value
//   an animation drives never starts a CSS transition.
// - An overshooting easing (cubic-bezier with y outside [0,1]) extrapolates
//   past the end keyframes, as on the web; opacity and alpha stay clamped.
// - bro.time: pause freezes playback in place; scale stretches it, identical
//   behavior to CSS transitions.
// - Headless: advanceTime(ms) advances animations deterministically;
//   getComputedStyle() reads the interpolated values; `await anim.finished`
//   works with top-level await.
// - transform/opacity-only animations are compositor-promoted (no base
//   re-record per frame), same as CSS transitions/animations.

// ── Common idioms ────────────────────────────────────────────────────────────

// Fire-and-forget entrance
card.animate([{ opacity: 0, transform: 'translateY(8px)' },
              { opacity: 1, transform: 'translateY(0px)' }],
             { duration: 180, easing: 'ease-out' });

// Await completion before removing
await note.animate([{ opacity: 1 }, { opacity: 0 }],
                   { duration: 200, fill: 'forwards' }).finished;
note.remove();

// Interruptible hover pulse
let pulse = null;
btn.addEventListener('mouseenter', () => {
    if (pulse) pulse.cancel();
    pulse = btn.animate([{ transform: 'scale(1)' }, { transform: 'scale(1.06)' }],
                        { duration: 120, fill: 'forwards' });
});
btn.addEventListener('mouseleave', () => {
    if (pulse) pulse.reverse();
});
