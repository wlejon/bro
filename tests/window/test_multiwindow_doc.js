// bro.window.open — secondary windows hosting REAL documents (multiwindow v1,
// chunk 2). Chunk 1's test covers the window lifecycle; this one covers the
// document half: an isolated realm built from `src`, rendered into the window,
// observable through the parent-side handle's capture().
//
// capture() is the only channel: realms are isolated by construction, so every
// assertion about the child is a pixel probe. The child paints five 50px
// stripes (see tests/test_app/multiwin_doc/index.html) — sample them at
// x = 25/75/125/175/225.

globalThis.__parentMarker = 1;

// Sample the pixel at (x, y) of an ImageData as [r, g, b].
function px(shot, x, y) {
    const i = (y * shot.width + x) * 4;
    return [shot.data[i], shot.data[i + 1], shot.data[i + 2]];
}
function isGreen(p) { return p[0] < 64 && p[1] > 192 && p[2] < 64; }
function isBlue(p)  { return p[0] < 64 && p[1] < 64 && p[2] > 192; }
function rgb(p)     { return p.join(','); }

// ---- open with a real src → 'load' fires -----------------------------------
const win = bro.window.open('multiwin_doc', { width: 250, height: 100,
                                              title: 'Doc window' });
let loadCount = 0;
let loadTargetOk = null;
win.addEventListener('load', (ev) => {
    loadCount++;
    loadTargetOk = (ev.target === win) && (ev.type === 'load');
});
assert(loadCount === 0, 'load waits for the drain');
flush();
assert(loadCount === 1, "exactly one 'load' after the create drain, got " + loadCount);
assert(loadTargetOk === true, "load event has type 'load' and target = handle");
assert(win.closed === false, 'window stays open after load');

// ---- capture() renders the child's document --------------------------------
let shot = win.capture();
assert(shot, 'capture() returns pixels once the document has loaded');
assert(shot.width === 250 && shot.height === 100,
       'capture is window-sized: ' + shot.width + 'x' + shot.height);

// Stripe 1: the child cannot see the parent realm's globals.
assert(isGreen(px(shot, 25, 50)),
       'child realm is isolated from the parent (stripe rgb ' + rgb(px(shot, 25, 50)) + ')');
// ...and the parent cannot see the child's.
assert(typeof __childMarker === 'undefined',
       "parent realm does not see the child's globals");

// Stripe 2: WebGL is refused in a secondary window (getContext → null).
assert(isGreen(px(shot, 75, 50)),
       'WebGL in a host realm returns null (stripe rgb ' + rgb(px(shot, 75, 50)) + ')');

// Stripe 3: a 2D canvas in the child actually paints.
assert(isBlue(px(shot, 125, 50)),
       '2D canvas renders in a host realm (stripe rgb ' + rgb(px(shot, 125, 50)) + ')');

// Stripe 4: the child's own timers have not fired yet (50ms setTimeout).
assert(!isGreen(px(shot, 175, 50)), "child timer hasn't fired before advanceTime");

// ---- child timers run under virtual time -----------------------------------
advanceTime(120);
shot = win.capture();
assert(isGreen(px(shot, 175, 50)),
       'child setTimeout fired and repainted (stripe rgb ' + rgb(px(shot, 175, 50)) + ')');

// ---- resize → child innerWidth updates + re-renders at the new size ---------
assert(!isGreen(px(shot, 225, 50)), 'resize stripe is unset before the resize');
win.setSize(300, 150);
flush();
shot = win.capture();
assert(shot.width === 300 && shot.height === 150,
       'capture follows the new window size: ' + shot.width + 'x' + shot.height);
assert(isGreen(px(shot, 225, 50)),
       'child saw the resize event with the new innerWidth (stripe rgb ' +
       rgb(px(shot, 225, 50)) + ')');
// Everything else still renders after the re-layout at the new size.
assert(isBlue(px(shot, 125, 50)), '2D canvas survives a resize');

// ---- the nested <iframe> in the child was skipped, not crashed --------------
// Reaching here at all is the assertion: the child document contains an
// <iframe src="iframe_child">, which v1 warns about and leaves unloaded.
assert(win.closed === false, 'nested <iframe> in a host document does not crash');

// ---- bro.window is scoped to THIS window inside the child realm -------------
// (Observable from the parent only in that the child's own getSize matches
// what we set; the scoped surface itself is exercised by the child's realm.)
const size = win.getSize();
assert(size.width === 300 && size.height === 150,
       'handle size after resize: ' + size.width + 'x' + size.height);

// ---- a src that does not resolve closes the handle cleanly ------------------
const bad = bro.window.open('no_such_app_at_all', { width: 40, height: 30 });
let badClosed = 0;
bad.addEventListener('close', () => { badClosed++; });
flush();
assert(bad.closed === true, 'a window whose src fails to load closes itself');
assert(badClosed === 1, 'and fires exactly one close event, got ' + badClosed);
assert(bad.capture() === null, 'capture() on a closed handle returns null');

// ---- close → full teardown; capture after close is clean -------------------
win.close();
flush();
assert(win.closed === true, 'window closed');
assert(win.capture() === null, 'capture() after close returns null, not stale pixels');

// ---- teardown on parent reload ---------------------------------------------
// performAppReload destroys every host with the dying realm. Re-open, then let
// the suite's own teardown exercise the destroy path; assert here that a fresh
// window still loads after the earlier churn (no leaked registry state).
const win2 = bro.window.open('multiwin_doc', { width: 250, height: 100 });
flush();
assert(win2.closed === false, 'a window opens cleanly after prior open/close churn');
const shot2 = win2.capture();
assert(shot2 && isGreen(px(shot2, 25, 50)), 'and renders its document');
win2.close();
flush();
assert(win2.closed === true, 'final window closed');

console.log('multiwindow doc OK');
