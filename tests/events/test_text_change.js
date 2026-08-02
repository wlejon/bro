// The `change` event of a text control.
//
// A text field had none: `input` fired per keystroke and `change` fired for a
// checkbox, a radio, a range and a colour, so every application control written
// the way HTML says to write one — read the value on change — was dead in the
// window and alive only in a test that dispatched the event by hand. The
// symptom is not a missing event, it is a field somebody typed a path into and
// a button beside it that says the path was never given.
//
// What is asserted here is the rule rather than the plumbing: `change` reports
// a *departure from an edited field*. So the shapes that must fire nothing are
// as much of the test as the ones that must — a field left untouched, a field
// typed in and undone back to what it was, a value a script wrote.

const root = document.getElementById('root');

const SDLK_RETURN = 0x0d;
const SDLK_ESCAPE = 0x1b;
const SDLK_TAB = 0x09;
const SDLK_BACKSPACE = 0x08;
const SDLK_END = 0x4000004d;

root.innerHTML =
    '<input id="a" type="text">' +
    '<input id="b" type="text">' +
    '<button id="btn">Go</button>' +
    '<textarea id="ta"></textarea>';
flush();

const a = document.getElementById('a');
const b = document.getElementById('b');
const btn = document.getElementById('btn');
const ta = document.getElementById('ta');

const log = [];
a.addEventListener('change', (e) => log.push('a:' + e.target.value));
b.addEventListener('change', (e) => log.push('b:' + e.target.value));
ta.addEventListener('change', (e) => log.push('ta:' + e.target.value));

// Two presses on one element within 5 px and 500 ms are a double-click, which
// takes a word, and three take the whole value — so consecutive presses on the
// same field alternate 40 px apart. Nothing here is testing selection; this is
// only how a test clicks twice in the same place without meaning it.
let bump = 0;
const clickOn = (el) => {
    bump = bump ? 0 : 1;
    const r = el.getBoundingClientRect();
    click(r.left + 6 + bump * 40, r.top + 5);
};
const key = (code) => { keyDown(code); keyUp(code); };
// Focus and type at the end, so where the press put the caret does not decide
// what the value comes out as.
const typeInto = (el, text) => { clickOn(el); key(SDLK_END); textInput(text); };

// ── typed, then clicked away ────────────────────────────────────────────────
typeInto(a, 'one');
assert(document.activeElement === a, 'a is focused');
assert(log.length === 0, 'nothing reported while typing (got ' + log.join() + ')');
clickOn(b);
assert(log.join() === 'a:one', 'a reported on the way out (got ' + log.join() + ')');

// ── the field that was left alone says nothing ─────────────────────────────
clickOn(a);
assert(log.join() === 'a:one', 'an untouched field is silent (got ' + log.join() + ')');

// ── typed and undone: the value never changed, so neither did anything ─────
log.length = 0;
typeInto(a, 'X');
key(SDLK_BACKSPACE);
clickOn(b);
assert(log.length === 0, 'a typed-then-deleted character reports nothing (got '
                         + log.join() + ')');

// ── a press inside the same field is not a departure ───────────────────────
log.length = 0;
typeInto(a, '!');
clickOn(a);   // move the caret, still in the same field
assert(log.length === 0, 'clicking within the focused field is silent (got '
                         + log.join() + ')');
clickOn(b);
assert(log.join() === 'a:one!', 'and it is still owed on the way out (got '
                                + log.join() + ')');

// ── Enter commits without leaving, and the departure after it is silent ────
log.length = 0;
typeInto(a, '?');
key(SDLK_RETURN);
assert(log.join() === 'a:one!?', 'Enter reports the edit (got ' + log.join() + ')');
assert(document.activeElement === a, 'Enter keeps the focus');
clickOn(b);
assert(log.join() === 'a:one!?', 'the same edit is not reported twice (got '
                                 + log.join() + ')');

// ── Escape leaves, and what stands is still an edit ────────────────────────
log.length = 0;
typeInto(a, 'esc');
key(SDLK_ESCAPE);
assert(log.join() === 'a:one!?esc', 'Escape reports the edit (got ' + log.join() + ')');

// ── Tab out ────────────────────────────────────────────────────────────────
log.length = 0;
typeInto(a, '-tab');
key(SDLK_TAB);
assert(log.join() === 'a:one!?esc-tab', 'Tab reports the edit (got ' + log.join() + ')');

// ── a script writing the value is not somebody changing it ─────────────────
log.length = 0;
clickOn(a);
a.value = 'written by a script';
clickOn(b);
assert(log.length === 0, 'a scripted value write reports nothing (got '
                         + log.join() + ')');

// ── .blur() and .focus() are departures like any other ─────────────────────
log.length = 0;
a.focus();
textInput('typed');
a.blur();
assert(log.join() === 'a:written by a scripttyped',
       'blur() reports the edit (got ' + log.join() + ')');

// ── the button beside the field still receives its click ───────────────────
// The whole point of the fix, and the half that is easy to lose: the change
// listener runs during the press, so the order the application sees is change
// first, then the click that reported it.
log.length = 0;
let clicks = 0;
btn.addEventListener('click', () => { clicks++; log.push('btn'); });
typeInto(a, '.mp4');
clickOn(btn);
assert(clicks === 1, 'the button was clicked (got ' + clicks + ')');
assert(log.join() === 'a:written by a scripttyped.mp4,btn',
       'the change arrived before the click (got ' + log.join() + ')');

// ── and still receives it when the change handler redraws it away ──────────
// The half a browser gets wrong. An application told something new redraws,
// which frees the very element the press is on its way to; a click that fires
// only where mousedown and mouseup land on one object is then lost, and the
// button works on the second press. Here the press is put back onto the
// control standing where it stood.
const box = document.createElement('div');
root.appendChild(box);
let rebuilds = 0, boxClicks = 0;
const build = () => {
    rebuilds++;
    box.innerHTML = '<button id="go">Go</button>';
    flush();
    document.getElementById('go').addEventListener('click', () => { boxClicks++; });
};
build();
a.addEventListener('change', build);

typeInto(a, '/x');
clickOn(document.getElementById('go'));
assert(rebuilds === 2, 'the change handler redrew (got ' + rebuilds + ')');
assert(boxClicks === 1, 'the redrawn button got the click (got ' + boxClicks + ')');

// ── unless what is standing there is a different control ───────────────────
// The safety half: a press must never be handed to something the person did
// not aim at, so a stand-in is only accepted where the tag and the words match.
let elseClicks = 0;
const buildOther = () => {
    box.innerHTML = '<button id="other">Delete everything</button>';
    flush();
    document.getElementById('other').addEventListener('click', () => { elseClicks++; });
};
a.removeEventListener('change', build);
a.addEventListener('change', buildOther);
typeInto(a, '/y');
clickOn(document.getElementById('go'));
assert(elseClicks === 0, 'a different control does not inherit the press (got '
                         + elseClicks + ')');

// ── a textarea reports the same way, and Enter is a newline there ──────────
log.length = 0;
clickOn(ta);
textInput('line');
key(SDLK_RETURN);
assert(log.length === 0, 'Enter in a textarea is a newline, not a commit (got '
                         + log.join() + ')');
textInput('two');
clickOn(a);
assert(log.length === 1 && log[0].indexOf('ta:line') === 0,
       'the textarea reported on the way out (got ' + log.join() + ')');
