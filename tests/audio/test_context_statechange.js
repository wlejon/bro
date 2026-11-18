// BaseAudioContext is an EventTarget: addEventListener / removeEventListener /
// dispatchEvent and `onstatechange`, with 'statechange' fired on the tick after
// suspend(), resume() or close() changes `state` (Web Audio 1.0, as Chromium
// has it). Tone.js's Context subscribes to it while it is built, so a missing
// addEventListener stopped Tone.js at load.

const ctx = new AudioContext();
assert(typeof ctx.addEventListener === 'function', 'AudioContext.addEventListener exists');
assert(typeof ctx.removeEventListener === 'function', 'removeEventListener exists');
assert(typeof ctx.dispatchEvent === 'function', 'dispatchEvent exists');
assert(ctx.onstatechange === null, 'onstatechange starts null');

const seen = [];
const listener = (e) => seen.push('listener:' + e.type + ':' + ctx.state);
ctx.addEventListener('statechange', listener);
ctx.onstatechange = (e) => seen.push('handler:' + e.type);

ctx.suspend();
assert(ctx.state === 'suspended', 'suspend() suspends');
assert(seen.length === 0, 'statechange is queued, not fired inside suspend()');
advanceTime(20);
assert(seen.join() === 'handler:statechange,listener:statechange:suspended',
       'statechange fires on the tick, got ' + seen.join());

seen.length = 0;
ctx.suspend();
advanceTime(20);
assert(seen.length === 0, 'suspend() of a suspended context changes nothing and fires nothing');

ctx.removeEventListener('statechange', listener);
ctx.resume();
advanceTime(20);
assert(ctx.state === 'running' && seen.join() === 'handler:statechange',
       'a removed listener is not called, got ' + seen.join());

seen.length = 0;
let once = 0;
ctx.addEventListener('ping', () => once++, { once: true });
assert(ctx.dispatchEvent(new Event('ping')) === true, 'dispatchEvent answers true for an uncancelled event');
ctx.dispatchEvent(new Event('ping'));
assert(once === 1, 'a once listener runs once, got ' + once);

ctx.close();
advanceTime(20);
assert(ctx.state === 'closed' && seen.join() === 'handler:statechange', 'close() fires statechange, got ' + seen.join());

console.log('test_context_statechange: OK');
