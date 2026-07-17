// Post-reload assertions for the top-level location.reload() test.
// Driven as:  bro-headless tests/engine/reload_app verify.js
//
// The app called location.reload() during its first run (engine
// construction); the headless driver drains the queued reload before
// evaluating this script — so THIS realm is the app's second run. (This file
// is deliberately not named test_*.js: the suite runner drives it indirectly
// through test_location_reload_toplevel.js, which spawns the child process.)

assert(document.title === 'fresh-realm',
       'old realm state must not survive reload, title=' + document.title);
assert(typeof globalThis.__reloadCanary === 'undefined', 'canary global gone');
assert(document.readyState === 'complete',
       'readyState is complete after reload, got ' + document.readyState);
assert(document.getElementById('stage').textContent === 'second run',
       'reloaded document re-ran its script against a fresh DOM');

// The first realm registered a 5ms interval that stamps process.env. Like
// the web, the old realm keeps running until the reload commits (the driver's
// drain point), so it may legitimately have fired BEFORE the swap — reset the
// stamp, advance well past the interval, and assert it never fires AFTER:
// that is what "timers do not survive the reload" means.
process.env.BRO_RELOAD_STALE = '';
advanceTime(200);
flush();
assert(!process.env.BRO_RELOAD_STALE,
       'old realm timers must not survive reload');

// The fresh realm is fully functional: timers run, DOM mutates, layout sees it.
let ticked = false;
setTimeout(() => { ticked = true; }, 10);
advanceTime(50);
assert(ticked, 'fresh realm timers run');

console.log('TOPLEVEL_RELOAD_OK');
