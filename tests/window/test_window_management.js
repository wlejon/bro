// bro.window — runtime window management surface.
// Exercises src/js/window_bindings.cpp (installBroWindowBindings) and
// src/platform/sdl_window.cpp. Headless runs against the hidden window:
// flag/limit setters round-trip (pure window state), while state-affecting
// ops (minimize/maximize/restore/setPosition/moveToDisplay) no-op so tests
// can never disturb the window the pipeline renders through.

assert(typeof bro === 'object', 'bro namespace exists');
assert(typeof bro.window === 'object', 'bro.window exists');

// ---- state ----------------------------------------------------------------
assert(bro.window.state === 'normal', 'hidden window starts normal, got ' + bro.window.state);

// ---- borderless round-trip ------------------------------------------------
assert(bro.window.borderless === false, 'borderless defaults false');
bro.window.borderless = true;
assert(bro.window.borderless === true, 'borderless set true round-trips');
bro.window.borderless = false;
assert(bro.window.borderless === false, 'borderless set false round-trips');

// ---- alwaysOnTop round-trip ----------------------------------------------
assert(bro.window.alwaysOnTop === false, 'alwaysOnTop defaults false');
bro.window.alwaysOnTop = true;
assert(bro.window.alwaysOnTop === true, 'alwaysOnTop set true round-trips');
bro.window.alwaysOnTop = false;
assert(bro.window.alwaysOnTop === false, 'alwaysOnTop set false round-trips');

// ---- min/max size round-trip ---------------------------------------------
let min = bro.window.getMinSize();
assert(min.width === 0 && min.height === 0, 'min size starts unconstrained');
bro.window.setMinSize(320, 240);
min = bro.window.getMinSize();
assert(min.width === 320 && min.height === 240,
       'min size round-trips: ' + min.width + 'x' + min.height);
bro.window.setMinSize(0, 0);
min = bro.window.getMinSize();
assert(min.width === 0 && min.height === 0, 'min size cleared');

let max = bro.window.getMaxSize();
assert(max.width === 0 && max.height === 0, 'max size starts unconstrained');
bro.window.setMaxSize(1600, 900);
max = bro.window.getMaxSize();
assert(max.width === 1600 && max.height === 900,
       'max size round-trips: ' + max.width + 'x' + max.height);
bro.window.setMaxSize(0, 0);
max = bro.window.getMaxSize();
assert(max.width === 0 && max.height === 0, 'max size cleared');

// ---- position query shape -------------------------------------------------
const pos = bro.window.getPosition();
assert(typeof pos === 'object' && pos !== null, 'getPosition returns object');
assert(typeof pos.x === 'number' && Number.isInteger(pos.x), 'position.x is integer');
assert(typeof pos.y === 'number' && Number.isInteger(pos.y), 'position.y is integer');

// ---- state-affecting ops are safe no-ops in headless ----------------------
bro.window.minimize();
assert(bro.window.state === 'normal', 'minimize no-ops headless');
bro.window.maximize();
assert(bro.window.state === 'normal', 'maximize no-ops headless');
bro.window.restore();
assert(bro.window.state === 'normal', 'restore no-ops headless');
bro.window.setPosition(10, 20); // must not throw or move anything

console.log('bro.window management OK');
