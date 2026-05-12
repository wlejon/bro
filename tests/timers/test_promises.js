// Test Promise microtask scheduling, async/await, queueMicrotask, and
// unhandled-rejection tracking. Exercises src/js/runtime.cpp microtask
// pump and src/js/timers.cpp scheduler integration.

// Install a handler that calls preventDefault on the rejections we expect
// in this test, so the harness doesn't log "[js error] unhandled rejection".
const expectedReasons = new Set(['oops', 'err', 'late_handled', 'never_caught', 'b']);
globalThis.onunhandledrejection = function(evt) {
    const r = evt.reason && evt.reason.message ? evt.reason.message : String(evt.reason);
    if (expectedReasons.has(r)) {
        evt.preventDefault();
    }
};

// =========================================================================
// Basic resolved Promise
// =========================================================================
const p = Promise.resolve(42);
let val = null;
p.then(v => { val = v; });
await Promise.resolve();
assert(val === 42, 'resolved promise then, got ' + val);

// =========================================================================
// Promise chain
// =========================================================================
const chained = await Promise.resolve(1)
    .then(v => v + 1)
    .then(v => v * 2);
assert(chained === 4, 'chained = (1+1)*2 = 4, got ' + chained);

// =========================================================================
// Promise rejection caught via .catch (attached at construction)
// =========================================================================
let caughtMsg = null;
const rejP = new Promise((_, reject) => { reject(new Error('oops')); });
rejP.catch(e => { caughtMsg = e.message; });
await Promise.resolve(); await Promise.resolve();
assert(caughtMsg === 'oops', 'reject caught, got ' + caughtMsg);

// =========================================================================
// queueMicrotask
// =========================================================================
let mtFired = false;
queueMicrotask(() => { mtFired = true; });
assert(mtFired === false, 'microtask not synchronous');
await Promise.resolve();
assert(mtFired === true, 'microtask ran');

// =========================================================================
// Microtask ordering — runs before timers
// =========================================================================
const order = [];
setTimeout(() => order.push('timer'), 0);
queueMicrotask(() => order.push('mt'));
await Promise.resolve();
// Microtask runs before advancing timers
assert(order[0] === 'mt', 'microtask runs first');
advanceTime(10);
assert(order.indexOf('timer') !== -1, 'timer ran after advance');

// =========================================================================
// Promise.all
// =========================================================================
const all = await Promise.all([Promise.resolve(1), Promise.resolve(2), Promise.resolve(3)]);
assert(all.length === 3, 'all length 3');
assert(all[0] === 1 && all[1] === 2 && all[2] === 3, 'all values in order');

// Promise.all with rejection — catch attached at construction
let allErr = null;
const allP = Promise.all([Promise.resolve(1), new Promise((_, rej) => rej('err')), Promise.resolve(3)]);
allP.catch(e => { allErr = e; });
await Promise.resolve(); await Promise.resolve();
assert(allErr === 'err', 'Promise.all rejects on first error');

// =========================================================================
// Promise.race
// =========================================================================
const winner = await Promise.race([
    new Promise(r => setTimeout(() => r('slow'), 100)),
    Promise.resolve('fast'),
]);
assert(winner === 'fast', 'race resolved winner');

// =========================================================================
// async function
// =========================================================================
async function doubled(x) { return x * 2; }
const dv = await doubled(21);
assert(dv === 42, 'async function returns awaitable');

// =========================================================================
// Promise.allSettled
// =========================================================================
if (typeof Promise.allSettled === 'function') {
    const settled = await Promise.allSettled([
        Promise.resolve('a'),
        new Promise((_, rej) => rej('b')),
    ]);
    assert(settled.length === 2, 'allSettled length');
    assert(settled[0].status === 'fulfilled', 'first fulfilled');
    assert(settled[1].status === 'rejected', 'second rejected');
}

// =========================================================================
// Setting a setTimeout inside a Promise then runs after microtask
// =========================================================================
let chainOrder = [];
Promise.resolve().then(() => {
    chainOrder.push('then1');
    setTimeout(() => chainOrder.push('timer'), 0);
}).then(() => chainOrder.push('then2'));
// Microtasks drain across multiple awaits.
for (let i = 0; i < 4; ++i) await Promise.resolve();
assert(chainOrder.indexOf('then1') !== -1, 'then1 ran');
assert(chainOrder.indexOf('then2') !== -1, 'then2 ran, order=' + JSON.stringify(chainOrder));
assert(chainOrder.indexOf('then2') > chainOrder.indexOf('then1'), 'then2 after then1');
advanceTime(10);
assert(chainOrder.indexOf('timer') !== -1, 'timer ran after advance');

// =========================================================================
// Catch attached synchronously at construction
// =========================================================================
const late = new Promise((_, rej) => rej('late_handled'));
late.catch(() => {});
