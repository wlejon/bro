// window.open — the web spelling of bro.window.open, in headless too.
//
// A URL with a scheme of its own (https:, mailto:, ...) is handed to the OS
// in a windowed run and returns null; headless never shells out. An
// app-relative src opens a bro window exactly as bro.window.open does —
// hidden in headless, the engine's policy for every secondary window — and
// returns its handle, so the multiwindow surface (load, capture, postMessage,
// close) is testable through the web API as well.

function px(shot, x, y) {
    const i = (y * shot.width + x) * 4;
    return [shot.data[i], shot.data[i + 1], shot.data[i + 2]];
}
function isGreen(p) { return p[0] < 64 && p[1] > 192 && p[2] < 64; }

assert(typeof window.open === 'function', 'window.open exists');
assert(open === window.open, 'the global open is window.open');

// ---- nothing to open --------------------------------------------------------
assert(window.open() === null, 'open() returns null');
assert(window.open(null) === null, 'open(null) returns null');
assert(window.open('') === null, "open('') returns null");
assert(window.open('about:blank') === null, 'about:blank returns null (no blank document)');

// ---- external URLs never shell out in headless -----------------------------
assert(window.open('https://example.com') === null, 'open(https) returns null');
assert(window.open('mailto:someone@example.com') === null, 'open(mailto) returns null');
assert(window.open('https://example.com', '_blank', 'noopener') === null, 'extra args accepted');

// ---- an app-relative src opens a bro window --------------------------------
const win = window.open('multiwin_doc', 'Doc window', 'width=250,height=100');
assert(win !== null && typeof win === 'object', 'window.open(src) returns a handle in headless');
assert(typeof win.id === 'number' && win.id > 0, 'handle.id is a positive number');
assert(win.closed === false, 'freshly opened handle is not closed');
let size = win.getSize();
assert(size.width === 250 && size.height === 100,
       'features string sets the size: ' + size.width + 'x' + size.height);

let loads = 0;
win.addEventListener('load', (ev) => { loads++; assert(ev.target === win, 'load target is the handle'); });
flush();
assert(loads === 1, "one 'load' after the drain, got " + loads);
assert(win.closed === false, 'still open after load');

// The child's document really rendered (multiwin_doc paints a green stripe
// at x = 25).
const shot = win.capture();
assert(shot && shot.width === 250 && shot.height === 100, 'capture() returns the window pixels');
assert(isGreen(px(shot, 25, 50)), 'the opened window renders its document: ' + px(shot, 25, 50).join(','));

win.close();
flush();
assert(win.closed === true, 'close() closes it');

// ---- an options object is accepted as features, like bro.window.open's -----
const win2 = window.open('multiwin_doc', '_blank', { width: 120, height: 80 });
assert(win2 && win2.getSize().width === 120 && win2.getSize().height === 80,
       'object features set the size');
win2.close();
flush();

// ---- noopener: the window opens, the caller gets null ----------------------
const before = bro.window.list ? bro.window.list().length : -1;
assert(window.open('multiwin_doc', '_blank', 'noopener,width=60,height=40') === null,
       'noopener returns null');

// ---- a src that does not resolve closes itself at the drain ----------------
const bad = window.open('no_such_app_dir_here');
assert(bad !== null, 'an unresolvable src still returns a handle up front');
flush();
assert(bad.closed === true, 'and it closes at the drain');

console.log('window.open OK');
