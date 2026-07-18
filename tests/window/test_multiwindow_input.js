// Per-window input routing (multiwindow v1, chunk 3).
//
// Every headless input seam takes an optional trailing `windowId`: omitted (or
// 0) means the main window and every pre-multiwindow test keeps working; a
// bro.window.open() handle id routes the event into THAT window's document.
// This test drives the whole matrix through a secondary window and asserts,
// for each event kind, that (a) the child saw it and (b) the parent did not.
//
// Realms are isolated, so every child-side assertion is a pixel probe through
// win.capture(). The child paints six 50px probe bands along the bottom row
// (see tests/test_app/multiwin_input/index.html), sampled at x = 25/75/.../275,
// y = 190.

function px(shot, x, y) {
    const i = (y * shot.width + x) * 4;
    return [shot.data[i], shot.data[i + 1], shot.data[i + 2]];
}
function isGreen(p) { return p[0] < 64 && p[1] > 192 && p[2] < 64; }
function rgb(p) { return p.join(','); }
// Probe band n (1-based) of the child's bottom row.
function probe(shot, n) { return px(shot, 25 + (n - 1) * 50, 190); }

// ---- parent-side "did NOT happen" instrumentation --------------------------
// Anything that leaks out of the child window shows up here.
let parentClicks = 0, parentKeys = 0, parentText = 0, parentWheels = 0,
    parentDrops = 0;
document.addEventListener('click', () => { parentClicks++; });
document.addEventListener('keydown', () => { parentKeys++; });
document.addEventListener('input', () => { parentText++; });
document.addEventListener('wheel', () => { parentWheels++; });
document.addEventListener('drop', () => { parentDrops++; });

// A parent-side text input at the same coordinates the child's field occupies,
// so "typing into the child" has a plausible wrong target to hit.
const root = document.getElementById('root') || document.body;
root.innerHTML =
    '<input id="parentField" type="text" value="" ' +
    'style="position:absolute;left:0;top:60px;width:200px;height:30px">' +
    '<div id="parentHover" style="position:absolute;left:100px;top:0;' +
    'width:100px;height:60px;cursor:crosshair"></div>';
flush();

const win = bro.window.open('multiwin_input', { width: 300, height: 200,
                                                title: 'Input window' });
flush();
assert(win.closed === false, 'input window opened');
assert(win.capture(), 'child document rendered');

// ---------------------------------------------------------------------------
// Click: hits the child's element, not the parent's
// ---------------------------------------------------------------------------
click(50, 30, 0, win.id);
let shot = win.capture();
assert(isGreen(probe(shot, 1)),
       'click landed on the child window\'s #btn (probe ' + rgb(probe(shot, 1)) + ')');
assert(parentClicks === 0,
       'the parent document saw no click, got ' + parentClicks);

// ...and a click in the MAIN window still works, unaffected.
click(50, 75);
assert(parentClicks === 1,
       'main-window click still reaches the app document, got ' + parentClicks);

// ---------------------------------------------------------------------------
// Hover: :hover resolves per window
// ---------------------------------------------------------------------------
// The child's #hovertarget is a 100x60 box at (100, 0) that turns green on
// :hover. Sample it directly rather than through a probe band.
mouseMove(150, 30, win.id);
shot = win.capture();
assert(isGreen(px(shot, 150, 30)),
       'child :hover resolved against the child\'s own hover target (' +
       rgb(px(shot, 150, 30)) + ')');

// Moving the pointer out again drops the child's :hover.
mouseMove(20, 120, win.id);
shot = win.capture();
assert(!isGreen(px(shot, 150, 30)), 'child :hover cleared when the pointer left');

// ---------------------------------------------------------------------------
// Cursor: resolved per window
// ---------------------------------------------------------------------------
// Park the MAIN window's pointer over a crosshair element first, so a leaking
// child cursor would be visible as a change here.
mouseMove(150, 30);
assert(currentCursor() === 'crosshair',
       'main window cursor from its own hover target, got ' + currentCursor());

mouseMove(50, 30, win.id);   // over the child's cursor:pointer #btn
assert(currentCursor(win.id) === 'pointer',
       'child window resolves its own cursor, got ' + currentCursor(win.id));
assert(currentCursor() === 'crosshair',
       'main window cursor untouched by the child, got ' + currentCursor());
// The no-arg form is exactly windowId 0.
assert(currentCursor() === currentCursor(0), 'currentCursor() === currentCursor(0)');

// ---------------------------------------------------------------------------
// Keyboard + focus: typing goes to the child's focused control
// ---------------------------------------------------------------------------
click(100, 75, 0, win.id);       // focus the child's #field
textInput('h', win.id);
textInput('i', win.id);
shot = win.capture();
assert(isGreen(probe(shot, 2)),
       'child input value became "hi" (probe ' + rgb(probe(shot, 2)) + ')');
assert(isGreen(probe(shot, 5)),
       'child document.activeElement is its own field (probe ' +
       rgb(probe(shot, 5)) + ')');
assert(parentText === 0, 'the parent input received nothing, got ' + parentText);
assert(document.getElementById('parentField').value === '',
       'parent field is still empty, got "' +
       document.getElementById('parentField').value + '"');

// keydown routed to the child, not the parent.
keyDown(97 /* SDLK_A */, 0, 0, false, win.id);
keyUp(97, 0, 0, win.id);
assert(parentKeys === 0, 'the parent saw no keydown, got ' + parentKeys);

// Focus is independent: the main window still focuses its own field.
click(50, 75);
assert(document.activeElement === document.getElementById('parentField'),
       'the main window focused its own input');
textInput('z');
assert(document.getElementById('parentField').value === 'z',
       'main-window typing still lands in the app document, got "' +
       document.getElementById('parentField').value + '"');
// ...and that did not disturb the child's focus or value.
shot = win.capture();
assert(isGreen(probe(shot, 5)), 'child keeps its own focus across main-window focus');

// ---------------------------------------------------------------------------
// IME: a composition commits into the child's text control
// ---------------------------------------------------------------------------
imeCompose('o', 1, win.id);
imeCommit('ok', win.id);
shot = win.capture();
assert(isGreen(probe(shot, 6)),
       'IME composition committed into the child window (probe ' +
       rgb(probe(shot, 6)) + ')');

// ---------------------------------------------------------------------------
// Wheel: scrolls the child's own overflow box
// ---------------------------------------------------------------------------
wheel(250, 40, 120, 0, win.id);
shot = win.capture();
assert(isGreen(probe(shot, 3)),
       'wheel scrolled the child\'s overflow element (probe ' +
       rgb(probe(shot, 3)) + ')');
assert(parentWheels === 0, 'the parent saw no wheel, got ' + parentWheels);

// ---------------------------------------------------------------------------
// Drop: routed into the child document
// ---------------------------------------------------------------------------
dropText(50, 120, 'payload', win.id);
shot = win.capture();
assert(isGreen(probe(shot, 4)),
       'drop reached the child document with its payload (probe ' +
       rgb(probe(shot, 4)) + ')');
assert(parentDrops === 0, 'the parent saw no drop, got ' + parentDrops);

// ---------------------------------------------------------------------------
// v1 refusals
// ---------------------------------------------------------------------------
// requestPointerLock from a host realm throws cleanly (the child caught it at
// load; reaching here with the window alive and rendering is the assertion).
assert(win.closed === false, 'pointer-lock refusal did not kill the child window');
// The child floods its background red if requestPointerLock did NOT throw.
shot = win.capture();
assert(px(shot, 295, 120)[0] < 64,
       'requestPointerLock threw in the host realm (background ' +
       rgb(px(shot, 295, 120)) + ')');
assert(document.pointerLockElement === null ||
       document.pointerLockElement === undefined,
       'no pointer lock was engaged by the child');

// An unknown window id is a no-op, not a crash — and above all it must not
// fall back to the main window.
const clicksBefore = parentClicks;
click(10, 10, 0, 99999);
keyDown(97, 0, 0, false, 99999);
wheel(10, 10, 100, 0, 99999);
assert(parentClicks === clicksBefore,
       'input at an unknown window id went nowhere, got ' + parentClicks);

// ---------------------------------------------------------------------------
// Teardown: input to a closed window is inert
// ---------------------------------------------------------------------------
const closedId = win.id;
win.close();
flush();
assert(win.closed === true, 'input window closed');
const clicksBeforeClosed = parentClicks;
click(50, 30, 0, closedId);
mouseMove(50, 30, closedId);
textInput('x', closedId);
assert(currentCursor(closedId) === 'default',
       'a closed window reports the default cursor');
assert(parentClicks === clicksBeforeClosed,
       'input to a closed window did not fall through to the parent, got ' +
       parentClicks);

console.log('multiwindow input OK');
