// Events at the window report the window as their target and current target.
//
// The window is an EventTarget with no Element behind it, and the engine's
// event carried only Element pointers, so an event dispatched AT the window
// read `target === null` and `currentTarget === null`, and an event bubbling
// from an element read the last ELEMENT as currentTarget in the window's own
// capture and bubble listeners, with the phase left over from that element.
//
// What the DOM says (2.9 dispatch), and what is asserted here:
//   * window.dispatchEvent(e): target and currentTarget are window, the phase
//     is AT_TARGET, and `this` is window;
//   * an element's event reaching the window: target is the element,
//     currentTarget is window, the phase is CAPTURING on the way down and
//     BUBBLING on the way up;
//   * after dispatch the caller's object keeps its target and has
//     currentTarget null and eventPhase NONE — also when no listener ran;
//   * engine-originated window events (postMessage) get the same answers.

const seen = [];
const who = (t) => t === window ? 'window'
                 : t === null ? 'null'
                 : t === document.body ? 'body'
                 : (t && t.tagName) ? t.tagName : typeof t;

function note(tag) {
    return function (e) {
        seen.push({ tag, target: who(e.target), current: who(e.currentTarget),
                    phase: e.eventPhase, self: this === window });
    };
}

function take(tag) {
    const hit = seen.filter(s => s.tag === tag);
    assert(hit.length === 1, tag + ' fired once, got ' + hit.length);
    return hit[0] || {};
}

// ── dispatched at the window ─────────────────────────────────────────────
{
    window.addEventListener('at-window', note('at-window'));
    window.addEventListener('at-window', note('at-window-capture'), true);
    const e = new Event('at-window');
    assert(e.target === null && e.currentTarget === null, 'an undispatched event has no target');
    window.dispatchEvent(e);

    const s = take('at-window');
    assert(s.target === 'window', 'target is window, got ' + s.target);
    assert(s.current === 'window', 'currentTarget is window, got ' + s.current);
    assert(s.phase === Event.AT_TARGET || s.phase === 2, 'phase is AT_TARGET, got ' + s.phase);
    assert(s.self === true, 'the listener runs with this === window');

    const c = take('at-window-capture');
    assert(c.phase === 2, 'a capture listener at the target also runs AT_TARGET, got ' + c.phase);
    assert(c.target === 'window' && c.current === 'window', 'capture listener sees window too');

    assert(e.target === window, 'after dispatch the event keeps target === window');
    assert(e.currentTarget === null, 'after dispatch currentTarget is null');
    assert(e.eventPhase === 0, 'after dispatch eventPhase is NONE, got ' + e.eventPhase);
}

// ── a CustomEvent, and a bare addEventListener call ──────────────────────
{
    addEventListener('custom-bare', note('custom-bare'));
    const e = new CustomEvent('custom-bare', { detail: { n: 1 } });
    dispatchEvent(e);
    const s = take('custom-bare');
    assert(s.target === 'window' && s.current === 'window', 'bare-registered listener sees window');
    assert(s.self === true, 'a bare-registered listener runs with this === window');
}

// ── no listener at all still sets target ─────────────────────────────────
{
    const e = new Event('nobody-listens');
    window.dispatchEvent(e);
    assert(e.target === window, 'target is set even when no listener ran');
    assert(e.currentTarget === null && e.eventPhase === 0, 'and the walk state is cleared');
}

// ── an element's event passing through the window ────────────────────────
{
    window.addEventListener('through', note('through-capture'), true);
    window.addEventListener('through', note('through-bubble'));
    document.body.addEventListener('through', note('through-body'));
    const e = new Event('through', { bubbles: true });
    document.body.dispatchEvent(e);

    const cap = take('through-capture');
    assert(cap.target === 'body', 'window capture: target is the element, got ' + cap.target);
    assert(cap.current === 'window', 'window capture: currentTarget is window, got ' + cap.current);
    assert(cap.phase === 1, 'window capture: CAPTURING_PHASE, got ' + cap.phase);

    const body = take('through-body');
    assert(body.current === 'body' && body.phase === 2, 'the element itself is AT_TARGET');

    const bub = take('through-bubble');
    assert(bub.target === 'body', 'window bubble: target is the element, got ' + bub.target);
    assert(bub.current === 'window', 'window bubble: currentTarget is window, got ' + bub.current);
    assert(bub.phase === 3, 'window bubble: BUBBLING_PHASE, got ' + bub.phase);

    assert(e.target === document.body, 'after dispatch target is the element');
    assert(e.currentTarget === null && e.eventPhase === 0, 'after dispatch the walk state is cleared');
}

// ── engine-originated: postMessage lands at the window ───────────────────
{
    window.addEventListener('message', note('message'));
    window.postMessage('hello', '*');
    for (let i = 0; i < 5 && !seen.some(s => s.tag === 'message'); i++) advanceTime(16);
    const s = take('message');
    assert(s.target === 'window' && s.current === 'window', 'message event target/currentTarget are window');
    assert(s.phase === 2, 'message event is AT_TARGET');
}

console.log('window event targets OK');
