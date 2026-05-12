// Test queueMicrotask ordering.

assert(typeof queueMicrotask === 'function', 'queueMicrotask exists');

const order = [];
order.push('sync-1');
queueMicrotask(() => { order.push('microtask-1'); });
queueMicrotask(() => { order.push('microtask-2'); });
order.push('sync-2');

// Microtasks should NOT have run yet
assert(order.length === 2, 'microtasks not yet run synchronously: ' + JSON.stringify(order));

flush();
// After flush, microtasks should have fired
assert(order.indexOf('microtask-1') >= 0, 'microtask-1 ran: ' + JSON.stringify(order));
assert(order.indexOf('microtask-2') >= 0, 'microtask-2 ran: ' + JSON.stringify(order));

// Order: microtask-1 before microtask-2
assert(order.indexOf('microtask-1') < order.indexOf('microtask-2'), 'FIFO order');

// Microtask runs before next macrotask
const order2 = [];
setTimeout(() => { order2.push('timer'); }, 0);
queueMicrotask(() => { order2.push('mt'); });
advanceTime(5);
flush();
assert(order2.indexOf('mt') < order2.indexOf('timer'),
    'microtask before timer: ' + JSON.stringify(order2));

// Microtask in microtask
let chain = '';
queueMicrotask(() => {
    chain += 'a';
    queueMicrotask(() => { chain += 'b'; });
});
flush();
assert(chain === 'ab', 'nested microtask: ' + chain);
