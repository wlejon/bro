// Ctrl+C / Ctrl+X / Ctrl+V reach the page as a keydown before the clipboard
// event, as in a browser, and a cancelled keydown stops the copy / cut /
// paste. Shift+C and Shift+X are the page's own chords: a keydown and no
// clipboard event (an app binds Mod+Shift+C to its own command); Shift+V
// still pastes. The engine used to send only the clipboard event, so a
// keydown listener never saw Mod+C, and Mod+Shift+C reached the page as a
// copy.

const SDLK_C = 0x63, SCAN_C = 6;
const SDLK_X = 0x78, SCAN_X = 27;
const SDLK_V = 0x76, SCAN_V = 25;
const CTRL = 0x0040, SHIFT = 0x0001;

const root = document.getElementById('root');
root.innerHTML = '<div id="d" tabindex="0">x</div>';
document.getElementById('d').focus();
flush();

const log = [];
const onKey = (e) => log.push('keydown ' + e.key + (e.ctrlKey ? ' ctrl' : '') + (e.shiftKey ? ' shift' : ''));
window.addEventListener('keydown', onKey);
for (const t of ['copy', 'cut', 'paste']) window.addEventListener(t, () => log.push(t));

const chord = (code, scan, mod) => { log.length = 0; keyDown(code, scan, mod); keyUp(code, scan, mod); flush(); return log.join(', '); };

let got = chord(SDLK_C, SCAN_C, CTRL);
assert(got === 'keydown c ctrl, copy', 'Ctrl+C: keydown then copy, got ' + got);
got = chord(SDLK_X, SCAN_X, CTRL);
assert(got === 'keydown x ctrl, cut', 'Ctrl+X: keydown then cut, got ' + got);
got = chord(SDLK_V, SCAN_V, CTRL);
assert(got === 'keydown v ctrl, paste', 'Ctrl+V: keydown then paste, got ' + got);

got = chord(SDLK_C, SCAN_C, CTRL | SHIFT);
assert(got === 'keydown C ctrl shift', 'Ctrl+Shift+C: a keydown, no copy, got ' + got);
got = chord(SDLK_X, SCAN_X, CTRL | SHIFT);
assert(got === 'keydown X ctrl shift', 'Ctrl+Shift+X: a keydown, no cut, got ' + got);
got = chord(SDLK_V, SCAN_V, CTRL | SHIFT);
assert(got === 'keydown V ctrl shift, paste', 'Ctrl+Shift+V still pastes, got ' + got);

// A cancelled keydown: no clipboard event.
const cancel = (e) => e.preventDefault();
window.addEventListener('keydown', cancel);
got = chord(SDLK_C, SCAN_C, CTRL);
assert(got === 'keydown c ctrl', 'a cancelled Ctrl+C does not copy, got ' + got);
got = chord(SDLK_V, SCAN_V, CTRL);
assert(got === 'keydown v ctrl', 'a cancelled Ctrl+V does not paste, got ' + got);
window.removeEventListener('keydown', cancel);
