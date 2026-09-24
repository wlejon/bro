// A focused form control's key default action runs AFTER keydown, and only
// when no listener cancelled it: Space on a checkbox with keydown
// preventDefault()ed does not toggle it. It used to toggle first and
// dispatch afterwards.

const SDLK_SPACE = 0x20;
const SDLK_BACKSPACE = 0x08;

const root = document.getElementById('root');
root.innerHTML = '<input type="checkbox" id="cb"><input type="text" id="t" value="abc">';
const cb = document.getElementById('cb');
flush();

let changes = 0;
cb.addEventListener('change', () => changes++);
cb.focus();
flush();
cb.click();
flush();
assert(cb.checked === true, 'click checks it');
assert(document.activeElement === cb, 'the checkbox took focus');

// Not cancelled: Space toggles, and keydown saw the pre-toggle state.
let seenChecked = null;
const peek = (e) => { if (e.keyCode === SDLK_SPACE) seenChecked = cb.checked; };
document.addEventListener('keydown', peek);
keyDown(SDLK_SPACE); keyUp(SDLK_SPACE);
flush();
assert(cb.checked === false, 'Space toggles an uncancelled checkbox');
assert(seenChecked === true, 'keydown runs before the toggle, saw checked=' + seenChecked);
document.removeEventListener('keydown', peek);

// Cancelled: nothing happens.
const before = changes;
const cancel = (e) => e.preventDefault();
document.addEventListener('keydown', cancel);
keyDown(SDLK_SPACE); keyUp(SDLK_SPACE);
flush();
assert(cb.checked === false, 'a cancelled keydown leaves the checkbox alone');
assert(changes === before, 'and fires no change');

// Same rule for a text field's editing keys.
const t = document.getElementById('t');
t.focus();
flush();
keyDown(SDLK_BACKSPACE); keyUp(SDLK_BACKSPACE);
flush();
assert(t.value === 'abc', 'a cancelled Backspace does not edit, got ' + t.value);
document.removeEventListener('keydown', cancel);
keyDown(SDLK_BACKSPACE); keyUp(SDLK_BACKSPACE);
flush();
assert(t.value.length === 2, 'an uncancelled Backspace edits, got ' + t.value);
