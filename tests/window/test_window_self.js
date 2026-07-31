// `self` — the WindowOrWorkerGlobalScope self-reference. Worker realms have
// always installed it (src/js/worker.cpp); the document realm did not, and
// library feature detection keys off it constantly. three.js in particular
// guards `if (typeof self !== 'undefined') animation.setContext(self)` in the
// WebGLRenderer constructor, so a missing `self` left the animation context
// null and made renderer.setAnimationLoop() / renderer.dispose() throw.

assert(typeof self !== 'undefined', 'self is defined in the document realm');
assert(self === globalThis, 'self === globalThis');
assert(self === window, 'self === window');
assert(self.self === self, 'self is reachable through itself');

// The properties libraries actually reach for through `self`.
assert(typeof self.requestAnimationFrame === 'function', 'self.requestAnimationFrame');
assert(typeof self.cancelAnimationFrame === 'function', 'self.cancelAnimationFrame');
assert(typeof self.setTimeout === 'function', 'self.setTimeout');

// Browser-vs-worker detection must still resolve to "browser": `window` is
// defined here, so the usual `typeof window === 'undefined'` worker probe
// keeps picking the document branch.
assert(typeof window !== 'undefined', 'window still defined (worker probes stay correct)');

// The rAF handle taken through `self` has to be the same clock the engine
// drives, not a separate one.
let fired = 0;
const id = self.requestAnimationFrame(() => { fired++; });
assert(typeof id === 'number', 'self.requestAnimationFrame returns a handle');
advanceTime(32);
flush();
assert(fired === 1, 'callback scheduled via self fired once');

// And cancelling through `self` has to reach the same queue.
let cancelledFired = 0;
const id2 = self.requestAnimationFrame(() => { cancelledFired++; });
self.cancelAnimationFrame(id2);
advanceTime(32);
flush();
assert(cancelledFired === 0, 'callback cancelled via self did not fire');

console.log('window self: OK');
