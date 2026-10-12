// A keydown the page cancels types nothing: the text the platform sends
// after it for the same key (SDL's TEXT_INPUT) is dropped, as the web drops
// a cancelled keydown's keypress and input. An app shortcut that moves the
// focus into a text field (a letter key that opens a name field) must not
// type its own letter there; it used to.

const SDLK_N = 0x6e, SCAN_N = 17;
const SDLK_A = 0x61, SCAN_A = 4;

const root = document.getElementById('root');
root.innerHTML = '<input type="text" id="t" value=""><input type="text" id="u" value=""><div id="d" tabindex="0">x</div>';
const t = document.getElementById('t');
const u = document.getElementById('u');
const d = document.getElementById('d');
flush();

let inputs = 0;
t.addEventListener('input', () => inputs++);
u.addEventListener('input', () => inputs++);
const press = (code, scan, ch) => { keyDown(code, scan, 0); textInput(ch); keyUp(code, scan, 0); flush(); };

// Not cancelled: the letter is typed.
t.focus();
flush();
press(SDLK_N, SCAN_N, 'n');
assert(t.value === 'n', 'an uncancelled key types its letter, got ' + JSON.stringify(t.value));

// Cancelled on the focused field: nothing typed, no input event.
const cancelN = (e) => { if (e.key === 'n') e.preventDefault(); };
document.addEventListener('keydown', cancelN);
const before = inputs;
press(SDLK_N, SCAN_N, 'n');
assert(t.value === 'n', 'a cancelled keydown types nothing, got ' + JSON.stringify(t.value));
assert(inputs === before, 'and fires no input');
// Only that key's text: the next key types as usual.
press(SDLK_A, SCAN_A, 'a');
assert(t.value === 'na', 'the next key types, got ' + JSON.stringify(t.value));
document.removeEventListener('keydown', cancelN);

// A shortcut on a non-field (the focused div) that moves the focus into a field and
// cancels its keydown: the field gets the focus, not the letter.
d.focus();
flush();
const shortcut = (e) => { if (e.key === 'n') { e.preventDefault(); u.focus(); } };
window.addEventListener('keydown', shortcut);
press(SDLK_N, SCAN_N, 'n');
assert(document.activeElement === u, 'the shortcut focused the field');
assert(u.value === '', 'without typing its own letter there, got ' + JSON.stringify(u.value));
window.removeEventListener('keydown', shortcut);
press(SDLK_A, SCAN_A, 'a');
assert(u.value === 'a', 'then typing goes in, got ' + JSON.stringify(u.value));

// Text with no keydown before it (an IME commit, an on-screen keyboard) is never dropped.
textInput('z');
flush();
assert(u.value === 'az', 'text on its own is typed, got ' + JSON.stringify(u.value));
