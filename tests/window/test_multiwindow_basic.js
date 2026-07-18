// bro.window.open — secondary window hosts (multiwindow v1, chunk 1).
//
// This chunk ships the WINDOW lifecycle only: open() → a real (hidden, in
// headless) secondary OS window with geometry/title control, close() / OS
// close → 'close' event + destroy. src is stored, no document yet — so the
// assertions here are all about handle state and lifecycle, not content.
//
// Lifecycle is queued: open()/close() take effect at the engine's idle drain,
// which flush() runs in headless. Headless policy mirrors bro.window's:
// secondary windows are always hidden, setSize round-trips (pure window
// state), setPosition no-ops (desk-dependent).

assert(typeof bro.window.open === 'function', 'bro.window.open exists');

// ---- bad arguments throw cleanly -------------------------------------------
let threw = false;
try { bro.window.open(); } catch (e) { threw = true; }
assert(threw, 'open() without src throws');
threw = false;
try { bro.window.open(''); } catch (e) { threw = true; }
assert(threw, 'open("") throws');

// ---- open → handle state ----------------------------------------------------
const win = bro.window.open('multiwin_child', {
    width: 320, height: 200, title: 'Palette',
});
assert(win && typeof win === 'object', 'open returns a handle');
assert(typeof win.id === 'number' && win.id > 0, 'handle.id is a positive number');
assert(win.closed === false, 'freshly opened handle is not closed');

// Pre-drain, geometry getters answer with the requested values.
let size = win.getSize();
assert(size.width === 320 && size.height === 200,
       'requested size before drain: ' + size.width + 'x' + size.height);

flush();  // drain → the (hidden) OS window materializes
assert(win.closed === false, 'still open after the create drain');
size = win.getSize();
assert(size.width === 320 && size.height === 200,
       'size after create round-trips: ' + size.width + 'x' + size.height);

// ---- resize round-trip (applies to hidden windows — pure window state) ------
win.setSize(400, 300);
size = win.getSize();
assert(size.width === 400 && size.height === 300,
       'setSize round-trips: ' + size.width + 'x' + size.height);

// ---- position: integer queries; setPosition no-ops on hidden windows --------
const posBefore = win.getPosition();
assert(Number.isInteger(posBefore.x) && Number.isInteger(posBefore.y),
       'getPosition returns integers');
win.setPosition(1234, 567);  // must not throw or move the hidden window
const posAfter = win.getPosition();
assert(posAfter.x === posBefore.x && posAfter.y === posBefore.y,
       'setPosition no-ops on a hidden window');

// ---- title + focus are safe no-throws ---------------------------------------
win.setTitle('renamed');
win.focus();

// ---- a second window: distinct ids, registry holds N ------------------------
const win2 = bro.window.open('multiwin_child', { width: 100, height: 80 });
assert(win2.id !== win.id, 'each open() mints a distinct id');
flush();
assert(win2.closed === false, 'second window open');
const size2 = win2.getSize();
assert(size2.width === 100 && size2.height === 80,
       'second window keeps its own size: ' + size2.width + 'x' + size2.height);

// ---- close() → closed flag + exactly one 'close'; double-close safe ---------
let closeCount = 0;
let closedInsideEvent = null;
let eventTargetOk = null;
win.addEventListener('close', (ev) => {
    closeCount++;
    closedInsideEvent = ev.target.closed;
    eventTargetOk = (ev.target === win) && (ev.type === 'close');
});
win.close();
win.close();  // double-close: coalesces
assert(win.closed === false, 'close is queued — not closed until the drain');
assert(closeCount === 0, 'close event waits for the drain');
flush();
assert(win.closed === true, 'closed after the drain');
assert(closeCount === 1, 'exactly one close event, got ' + closeCount);
assert(closedInsideEvent === true, 'handle.closed already true inside the listener');
assert(eventTargetOk === true, 'close event has type "close" and target = handle');
win.close();  // close after closed: no-op, no throw
flush();
assert(closeCount === 1, 'no extra close events after re-close');

// The other window is untouched by its sibling's close.
assert(win2.closed === false, 'sibling window unaffected by close');

// ---- removeEventListener -----------------------------------------------------
let removedFired = 0;
const removedFn = () => { removedFired++; };
win2.addEventListener('close', removedFn);
win2.removeEventListener('close', removedFn);
win2.close();
flush();
assert(win2.closed === true, 'second window closed');
assert(removedFired === 0, 'removed close listener does not fire');

// ---- open + close before any drain: window never materializes, still clean --
const win3 = bro.window.open('multiwin_child', {});
let c3 = 0;
win3.addEventListener('close', () => { c3++; });
win3.close();
flush();
assert(win3.closed === true, 'open+close within one drain closes cleanly');
assert(c3 === 1, 'and fires exactly one close event, got ' + c3);

// ---- open from an iframe realm is rejected with a clean error ----------------
// The child paints its body green if bro.window.open threw the deliberate
// main-realm error, red if it did not throw, blue on a different error
// (see tests/test_app/multiwin_child/index.html). Pixels are the only
// cross-realm channel this chunk.
const frame = document.createElement('iframe');
frame.setAttribute('src', 'multiwin_child');
frame.style.width = '60px';
frame.style.height = '40px';
document.body.appendChild(frame);
flush();
const shot = frame.capture();
assert(shot, 'iframe child sub-doc rendered');
const px = [shot.data[0], shot.data[1], shot.data[2]];
assert(px[0] < 64 && px[1] > 192 && px[2] < 64,
       'open() from an iframe realm throws the main-realm error (pixel rgb ' +
       px.join(',') + ')');

console.log('multiwindow basic OK');
