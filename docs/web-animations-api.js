/**
 * element.animate(), Web Animations API (commonly-used subset)
 *
 * Script-driven animations that ride the exact same machinery as CSS
 * transitions and @keyframes animations: interpolated values are injected
 * into computed style during style resolution (above both CSS transitions and
 * CSS animations in composite order), the clock is the engine's scaled
 * bro.time clock (bro.time.paused freezes them, bro.time.scale stretches
 * them), transform/opacity-only animations get the same compositor-layer
 * promotion, and headless advanceTime(ms) drives them deterministically.
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
 * DELIBERATE SIMPLIFICATIONS (vs the full spec):
 *   - Stacking: multiple animations on one element compose in creation
 *     order: the LAST-CREATED animation wins per property (no full
 *     composite-order machinery; composite modes other than "replace" are
 *     ignored).
 *   - commitStyles() / persist() / updatePlaybackRate() / ready promise /
 *     KeyframeEffect object / timeline objects are not implemented.
 *     animation.effect and animation.startTime are absent; `pending` is
 *     always false (play/pause apply immediately).
 *   - getAnimations() returns running/paused animations plus finished ones
 *     still holding a forwards fill (spec "relevant" ≈ same set).
 *   - Object-form keyframes distribute values evenly; an explicit `offset`
 *     list inside the object form is ignored (use the array form for
 *     explicit offsets). An `easing` array is applied cyclically across the
 *     merged keyframes.
 *   - easing accepts what the CSS transition engine parses: linear | ease |
 *     ease-in | ease-out | ease-in-out | cubic-bezier(...). steps() is not
 *     supported (falls back to ease). Unknown strings fall back to ease
 *     rather than throwing. The default is linear (the WAAPI default).
 *   - reverse() on an infinite animation seeks to 0 (spec throws).
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

// ── Enumeration ──────────────────────────────────────────────────────────────

el.getAnimations();        // Animation[] for this element (running/paused +
                           // finished-while-filling-forwards), creation order,
                           // identity-preserving (same objects you got back)
document.getAnimations();  // the same across the whole document

// ── Interplay ────────────────────────────────────────────────────────────────
//
// - Overrides inline style and the cascade while active, and sits above CSS
//   transitions AND CSS animations for the properties it animates.
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
