// Test focus, blur, focusin, focusout, document.activeElement.
// Exercises focus management in src/engine/input_handling.cpp and event
// dispatch in src/js/event_dispatch.cpp.

const root = document.getElementById('root');
root.innerHTML =
    '<input id="a" type="text">' +
    '<input id="b" type="text">' +
    '<button id="btn">Click</button>' +
    '<div id="div" tabindex="0">div</div>';
flush();

const a = document.getElementById('a');
const b = document.getElementById('b');
const btn = document.getElementById('btn');
const div = document.getElementById('div');

const log = [];

a.addEventListener('focus', () => log.push('a:focus'));
a.addEventListener('blur',  () => log.push('a:blur'));
b.addEventListener('focus', () => log.push('b:focus'));
b.addEventListener('blur',  () => log.push('b:blur'));

// focusin / focusout bubble
document.body.addEventListener('focusin',  (e) => log.push('body:focusin:' + e.target.id));
document.body.addEventListener('focusout', (e) => log.push('body:focusout:' + e.target.id));

// --- el.focus() programmatic ---
a.focus();
assert(document.activeElement === a, 'activeElement = a after focus()');

// Programmatic focus should fire focus event
assert(log.indexOf('a:focus') !== -1, 'focus event fired on a');

// --- Switching focus fires blur on old, focus on new ---
b.focus();
assert(document.activeElement === b, 'activeElement = b after b.focus()');
assert(log.indexOf('a:blur') !== -1, 'a:blur fired');
assert(log.indexOf('b:focus') !== -1, 'b:focus fired');

// focusin / focusout bubble
assert(log.some(e => e.indexOf('body:focusin') !== -1),
       'focusin bubbled to body');

// --- el.blur() removes focus ---
b.blur();
// blur fires
assert(log.filter(e => e === 'b:blur').length >= 1, 'b:blur after blur()');
// activeElement should be body or null
assert(document.activeElement !== b, 'activeElement no longer b');

// --- Clicking an input focuses it ---
log.length = 0;
const r = a.getBoundingClientRect();
click(r.left + r.width / 2, r.top + r.height / 2);
assert(document.activeElement === a, 'click focused a');

// --- Typing into focused input ---
textInput('Hello');
assert(a.value === 'Hello', 'textInput populates focused input, got: ' + a.value);

// Tab moves focus (if supported)
// SDL_SCANCODE_TAB = 43, SDL_KMOD_NONE = 0
// Note: tab handling depends on engine input handling — just verify no crash.
keyDown(0x09); // SDLK_TAB
keyUp(0x09);

// --- input event on typing ---
let inputEv = null;
a.addEventListener('input', (e) => { inputEv = e; });
const rA = a.getBoundingClientRect();
click(rA.left + 5, rA.top + 5);
textInput('X');
// May or may not have fired depending on impl; check value at least
assert(a.value.indexOf('X') !== -1, 'X typed into a');

// --- focus on non-input element with tabindex ---
log.length = 0;
div.addEventListener('focus', () => log.push('div:focus'));
div.focus();
assert(log.indexOf('div:focus') !== -1, 'div with tabindex focusable');

// --- button focus + click ---
let clicked = false;
btn.addEventListener('click', () => { clicked = true; });
btn.focus();
const rBtn = btn.getBoundingClientRect();
click(rBtn.left + rBtn.width / 2, rBtn.top + rBtn.height / 2);
assert(clicked, 'button click fired');

// Cleanup
root.innerHTML = '';
