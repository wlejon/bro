// bro.window — window properties (title, opacity, fullscreen, focused, flash)
// Exercises primary window and secondary window handle properties.

assert(typeof bro === 'object', 'bro namespace exists');
assert(typeof bro.window === 'object', 'bro.window exists');

// ---- Primary window title ----------------------------------------------------
const initialTitle = bro.window.title;
assert(typeof initialTitle === 'string', 'initial title is a string');
assert(bro.window.getTitle() === initialTitle, 'getTitle() matches title property');

bro.window.title = 'Hello Desktop';
assert(bro.window.title === 'Hello Desktop', 'setting title property round-trips');
assert(bro.window.getTitle() === 'Hello Desktop', 'getTitle() reflects updated title');

bro.window.setTitle('Custom Title 123');
assert(bro.window.title === 'Custom Title 123', 'setTitle() updates title property');
assert(bro.window.getTitle() === 'Custom Title 123', 'getTitle() returns updated title');

// ---- Primary window opacity --------------------------------------------------
const initialOpacity = bro.window.opacity;
assert(typeof initialOpacity === 'number', 'initial opacity is a number');
assert(initialOpacity >= 0.0 && initialOpacity <= 1.0, 'initial opacity in [0, 1]');

bro.window.opacity = 0.75;
assert(Math.abs(bro.window.opacity - 0.75) < 0.01, 'setting opacity property round-trips: ' + bro.window.opacity);
assert(Math.abs(bro.window.getOpacity() - 0.75) < 0.01, 'getOpacity() reflects updated opacity');

bro.window.setOpacity(0.5);
assert(Math.abs(bro.window.opacity - 0.5) < 0.01, 'setOpacity() updates opacity property');
assert(Math.abs(bro.window.getOpacity() - 0.5) < 0.01, 'getOpacity() returns updated opacity');

// Clamping: < 0 becomes 0, > 1 becomes 1
bro.window.opacity = -0.5;
assert(bro.window.opacity === 0.0, 'opacity clamps negative to 0.0, got ' + bro.window.opacity);
bro.window.opacity = 1.5;
assert(bro.window.opacity === 1.0, 'opacity clamps > 1 to 1.0, got ' + bro.window.opacity);

// Restore full opacity
bro.window.opacity = 1.0;

// ---- Primary window fullscreen -----------------------------------------------
assert(typeof bro.window.fullscreen === 'boolean', 'fullscreen is boolean');
assert(bro.window.fullscreen === false, 'starts not fullscreen');

bro.window.fullscreen = true;
assert(bro.window.fullscreen === true, 'fullscreen set true round-trips');
bro.window.setFullscreen(false);
assert(bro.window.fullscreen === false, 'setFullscreen(false) round-trips');

// ---- Primary window focused --------------------------------------------------
assert(typeof bro.window.focused === 'boolean', 'focused is boolean');
// Focused is read-only
const focusValBefore = bro.window.focused;
try {
    bro.window.focused = !focusValBefore;
} catch (e) {
    // strict mode throws on read-only assignment
}
assert(bro.window.focused === focusValBefore, 'focused property is read-only');

// ---- Primary window flash / requestAttention ---------------------------------
assert(typeof bro.window.flash === 'function', 'flash exists');
assert(typeof bro.window.requestAttention === 'function', 'requestAttention exists');
assert(bro.window.flash(true) === true, 'flash(true) succeeds');
assert(bro.window.flash(false) === true, 'flash(false) succeeds');
assert(bro.window.requestAttention(true) === true, 'requestAttention(true) succeeds');
assert(bro.window.requestAttention(false) === true, 'requestAttention(false) succeeds');

// ---- Secondary window handle properties -------------------------------------
const win = bro.window.open('multiwin_child', {
    width: 400, height: 300, title: 'Secondary Test Win',
});
assert(win && typeof win === 'object', 'secondary window handle created');
flush();

assert(win.getTitle() === 'Secondary Test Win', 'secondary window getTitle matches');
win.setTitle('Renamed Secondary');
assert(win.getTitle() === 'Renamed Secondary', 'secondary window setTitle round-trips');

assert(typeof win.getOpacity() === 'number', 'secondary window getOpacity is number');
win.setOpacity(0.8);
assert(Math.abs(win.getOpacity() - 0.8) < 0.01, 'secondary window setOpacity round-trips');

assert(typeof win.focused === 'boolean', 'secondary window focused is boolean');
assert(win.flash(true) === true, 'secondary window flash(true) succeeds');
assert(win.flash(false) === true, 'secondary window flash(false) succeeds');
assert(win.requestAttention(true) === true, 'secondary window requestAttention succeeds');

win.close();
flush();

console.log('bro.window properties OK');
