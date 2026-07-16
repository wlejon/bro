/**
 * Pointer Events + Touch Events — unified W3C input over mouse and touch
 *
 * bro dispatches W3C Pointer Events for every input device and W3C Touch
 * Events alongside them for touch, on top of the classic mouse events.
 * Handlers written for any of the three models work.
 *
 * ── The pointer table ──────────────────────────────────────────────────────
 *
 * Every pointer event carries { pointerId, pointerType, isPrimary, pressure }
 * in addition to the full MouseEvent surface (clientX/Y, buttons, offsets…):
 *
 *   mouse   pointerId 1 (always), pointerType "mouse", always primary.
 *           Synthesized just before each mouse event: pointerdown before
 *           mousedown, pointermove before mousemove, pointerup before
 *           mouseup. pressure reads 0.5 while any button is held, else 0.
 *
 *   touch   one pointer per finger. pointerIds are unique per contact,
 *           minted monotonically starting at 2 — they NEVER collide with the
 *           mouse's 1, and a lifted finger's id is not reused. The first
 *           finger of a contact set (a touch landing on an empty surface) is
 *           the primary pointer for its whole lifetime; fingers added while
 *           others are down are non-primary. pressure is the device pressure
 *           (1.0 where the platform doesn't report it), 0 on pointerup /
 *           pointercancel. buttons is 1 while the finger is down; button is
 *           0 on the down/up transitions and -1 on moves.
 *
 * Pointer events hit-test their contact point per event (per finger, for
 * touch). Touch does not have implicit capture in bro — capture it
 * explicitly from pointerdown (below) if you want the drag idiom.
 *
 * ── Per-pointer capture ────────────────────────────────────────────────────
 *
 * Element.setPointerCapture(pointerId) routes subsequent pointermove /
 * pointerup / pointercancel for THAT pointer to the element regardless of
 * the hit target (offsetX/Y recomputed against it). Each pointerId is
 * captured independently — one finger can be captured by a slider while a
 * second finger scrolls elsewhere. The capture auto-releases after
 * pointerup / pointercancel; gotpointercapture / lostpointercapture fire on
 * the holder. Capturing an inactive pointerId is a silent no-op (the web
 * throws NotFoundError). Mouse events are never retargeted by capture.
 *
 * ── Touch Events ───────────────────────────────────────────────────────────
 *
 * Each contact transition dispatches the pointer event first, then the touch
 * event (spec order):
 *
 *   finger down   pointerdown  → touchstart
 *   finger move   pointermove  → touchmove
 *   finger up     pointerup    → touchend
 *   aborted       pointercancel → touchcancel   (not cancelable)
 *
 * TouchEvent carries three TouchList lists of Touch objects:
 *   touches         every finger currently on the surface (a finger lifted
 *                   by this very event is already excluded)
 *   targetTouches   the subset that started on this event's target
 *   changedTouches  the contact(s) this event reports
 *
 * Touch.identifier equals the contact's pointerId, so the pointer and touch
 * streams correlate 1:1. Touch events always fire at the contact's
 * touchstart target for its whole lifetime (W3C targeting rule), even while
 * the pointer stream is captured elsewhere or the finger slides off the
 * element.
 *
 * ── Compat mouse events ────────────────────────────────────────────────────
 *
 * A TAP of the primary finger — down and up without travelling past the
 * ~10 px slop radius — synthesizes the classic mouse sequence after
 * touchend:
 *
 *   mousedown → mouseup → click
 *
 * with full focus semantics (a tap focuses an <input> exactly like a click)
 * and the rolling double-click streak (a quick double-tap yields dblclick).
 * Suppression follows the web rule: preventDefault() on pointerdown,
 * touchstart, or touchend cancels the compat sequence; so does dragging past
 * the slop, a pointercancel, or the finger being non-primary.
 *
 * Deviations from full browser compat behavior (deliberate, documented):
 *   * no mousemove/mouseover/mouseout synthesis from touch movement — only
 *     the tap sequence above is synthesized;
 *   * hover is mouse-only: touch never updates :hover styling and never
 *     fires mouseover/mouseenter/mouseleave/mouseout;
 *   * touch targets the app document only — system panels, native menus and
 *     engine overlays remain mouse-driven;
 *   * touch contacts never begin text-selection drags.
 */

// ── Pointer events (mouse + touch) ─────────────────────────────────────────

/**
 * @typedef {MouseEvent} PointerEvent
 * @property {number}  pointerId   - 1 for the mouse; unique >= 2 per touch contact
 * @property {"mouse"|"touch"} pointerType
 * @property {boolean} isPrimary   - mouse: always true; touch: first finger of the set
 * @property {number}  pressure    - 0..1 (0 on pointerup/pointercancel)
 * @property {number}  width       - contact width (currently 1)
 * @property {number}  height      - contact height (currently 1)
 */

el.addEventListener('pointerdown', (e) => {
    // Works identically for mouse presses and finger contacts.
    console.log(e.pointerType, e.pointerId, e.isPrimary, e.clientX, e.clientY);

    // The drag idiom: capture this pointer so the stroke keeps arriving
    // here after it leaves the element. Per-pointer: capture each finger
    // you care about.
    el.setPointerCapture(e.pointerId);
});
el.addEventListener('pointermove', (e) => { /* per-pointer moves */ });
el.addEventListener('pointerup', (e) => { /* capture auto-releases after this */ });
el.addEventListener('pointercancel', (e) => { /* gesture aborted (OS/palm) */ });

el.setPointerCapture(pointerId);      // route this pointer's events here
el.releasePointerCapture(pointerId);  // explicit release (holder only)
el.hasPointerCapture(pointerId);      // -> boolean

// ── Touch events ────────────────────────────────────────────────────────────

el.addEventListener('touchstart', (e) => {
    // e instanceof TouchEvent; lists are TouchList (length / item(i) / [i]).
    for (let i = 0; i < e.changedTouches.length; i++) {
        const t = e.changedTouches.item(i);
        console.log(t.identifier, t.clientX, t.clientY, t.force, t.target);
    }
    // Prevent the compat mousedown/mouseup/click for this contact:
    e.preventDefault();
});
el.addEventListener('touchmove', (e) => { /* fires at the touchstart target */ });
el.addEventListener('touchend', (e) => { /* e.touches excludes lifted fingers */ });
el.addEventListener('touchcancel', (e) => { /* not cancelable */ });

// ── Multi-touch example: two independent drags ──────────────────────────────

const strokes = new Map();   // pointerId -> stroke state
canvas.addEventListener('pointerdown', (e) => {
    if (e.pointerType !== 'touch' && e.pointerType !== 'mouse') return;
    canvas.setPointerCapture(e.pointerId);       // per-finger capture
    strokes.set(e.pointerId, [{ x: e.offsetX, y: e.offsetY }]);
});
canvas.addEventListener('pointermove', (e) => {
    const s = strokes.get(e.pointerId);
    if (s) s.push({ x: e.offsetX, y: e.offsetY });
});
canvas.addEventListener('pointerup', (e) => strokes.delete(e.pointerId));
canvas.addEventListener('pointercancel', (e) => strokes.delete(e.pointerId));

// ── Headless injection ──────────────────────────────────────────────────────
//
// The headless globals drive the same engine entry points as SDL finger
// events (the gamepad-seam pattern: injected below the JS API, above SDL),
// so pointer events, touch events, capture, and the compat mouse sequence
// all exercise the real pipeline. `id` is a caller-chosen contact id (the
// SDL finger-id analog): reuse one id for the move/up/cancel of one contact;
// distinct concurrent ids are distinct fingers. Coordinates are
// viewport-relative, like the mouse helpers. See docs/headless.md.

touchDown(1, 100, 100);        // finger 1 lands (optional 4th arg: pressure 0..1)
touchMove(1, 140, 100);        // finger 1 slides
touchDown(2, 300, 200);        // finger 2 lands while 1 is down (non-primary)
touchUp(2, 300, 200);          // finger 2 taps (no compat click — non-primary)
touchUp(1, 140, 100);          // finger 1 lifts (dragged — no compat click)
touchCancel(1, 0, 0);          // or: abort a contact (pointercancel/touchcancel)
