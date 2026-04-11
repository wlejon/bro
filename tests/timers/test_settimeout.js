// Test setTimeout with advanceTime

let fired = false;
let firedAt = 0;

setTimeout(() => {
    fired = true;
    firedAt = Date.now();
}, 100);

// Before advancing time, callback should not have fired
flush();
assert(!fired, 'not fired immediately');

// Advance time past the timeout
advanceTime(150);
assert(fired, 'fired after advanceTime(150)');

// clearTimeout prevents firing
let cleared = false;
const id = setTimeout(() => { cleared = true; }, 100);
clearTimeout(id);
advanceTime(200);
assert(!cleared, 'clearTimeout prevents callback');

// setTimeout with 0 delay fires on next tick
let zeroFired = false;
setTimeout(() => { zeroFired = true; }, 0);
advanceTime(1);
assert(zeroFired, 'setTimeout(fn, 0) fires after advanceTime(1)');
