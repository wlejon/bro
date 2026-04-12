// Test headless input simulation: keyDown, keyUp, textInput, wheel, resize.

var root = document.getElementById('root');

// ==========================================================================
// Keyboard: keyDown / keyUp
// ==========================================================================

// SDL keycodes: ASCII letters are their char codes (e.g. 'a' = 97)
var SDLK_A = 97;
var SDLK_RETURN = 0x0d;
var SDLK_ESCAPE = 0x1b;

var keydownFired = false;
var keyupFired = false;
var lastKeyEvent = null;

document.body.addEventListener('keydown', function(e) {
    keydownFired = true;
    lastKeyEvent = e;
});
document.body.addEventListener('keyup', function(e) {
    keyupFired = true;
});

// Fire a keydown for 'a'
keyDown(SDLK_A);
assert(keydownFired, 'keyDown fires keydown event');
assert(lastKeyEvent.key === 'a', 'keydown event.key is "a"');
assert(lastKeyEvent.repeat === false, 'keydown repeat is false');

// Fire keyup
keyUp(SDLK_A);
assert(keyupFired, 'keyUp fires keyup event');

// --- Repeat key ---
keydownFired = false;
lastKeyEvent = null;
keyDown(SDLK_A, 0, 0, true);  // repeat = true
assert(keydownFired, 'repeat keyDown fires');
assert(lastKeyEvent.repeat === true, 'repeat keydown has repeat=true');

// --- Modifier keys ---
keydownFired = false;
lastKeyEvent = null;
// SDL_KMOD_CTRL = 0x0040 | 0x0080 = 0x00C0, but KMOD_LCTRL = 0x0040
keyDown(SDLK_A, 0, 0x0040);
assert(lastKeyEvent.ctrlKey === true, 'ctrl modifier is set');
keyUp(SDLK_A, 0, 0x0040);

// --- Special keys ---
keydownFired = false;
lastKeyEvent = null;
keyDown(SDLK_RETURN);
assert(lastKeyEvent.key === 'Enter', 'Enter key maps correctly');
keyUp(SDLK_RETURN);

// Note: Escape is intercepted by system_toggle_settings and doesn't reach JS.
// Test Backspace instead.
var SDLK_BACKSPACE = 0x08;
keydownFired = false;
lastKeyEvent = null;
keyDown(SDLK_BACKSPACE);
assert(lastKeyEvent.key === 'Backspace', 'Backspace key maps correctly');
keyUp(SDLK_BACKSPACE);

// ==========================================================================
// textInput
// ==========================================================================

// Set up an input element to receive text
root.innerHTML = '<input id="inp" type="text">';
flush();
var inp = document.querySelector('#inp');

// Focus the input by clicking it
var r = inp.getBoundingClientRect();
click(r.left + r.width / 2, r.top + r.height / 2);

// Type text
textInput('hello');

// The input should have received the text
assert(inp.value === 'hello', 'textInput populates input value, got: ' + inp.value);

// ==========================================================================
// wheel
// ==========================================================================

var wheelFired = false;
var wheelDeltaY = 0;

root.innerHTML = '<div id="scrollbox" style="width:100px;height:100px;overflow:auto"><div style="height:500px">tall</div></div>';
flush();

document.body.addEventListener('wheel', function(e) {
    wheelFired = true;
    wheelDeltaY = e.deltaY;
});

var box = document.querySelector('#scrollbox');
var br = box.getBoundingClientRect();
wheel(br.left + 50, br.top + 50, 100);

assert(wheelFired, 'wheel() fires wheel event');
assert(wheelDeltaY !== 0, 'wheel event has deltaY');

// ==========================================================================
// resize
// ==========================================================================

// Check initial viewport
assert(typeof innerWidth === 'number', 'innerWidth is a number');
assert(typeof innerHeight === 'number', 'innerHeight is a number');
var oldW = innerWidth;
var oldH = innerHeight;

// Resize
resize(800, 600);

assert(innerWidth === 800, 'innerWidth updated after resize to 800, got: ' + innerWidth);
assert(innerHeight === 600, 'innerHeight updated after resize to 600, got: ' + innerHeight);

// Restore original size
resize(oldW, oldH);
assert(innerWidth === oldW, 'innerWidth restored');

// --- Cleanup ---
root.innerHTML = '';
