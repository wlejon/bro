// Test setInterval with advanceTime

let count = 0;
const id = setInterval(() => { count++; }, 50);

// Advance to trigger multiple firings
advanceTime(50);
assert(count >= 1, 'at least 1 after 50ms: ' + count);

advanceTime(50);
assert(count >= 2, 'at least 2 after 100ms: ' + count);

advanceTime(50);
assert(count >= 3, 'at least 3 after 150ms: ' + count);

// Clear interval stops further callbacks
clearInterval(id);
const countBefore = count;
advanceTime(200);
assert(count === countBefore, 'no more callbacks after clearInterval: ' + count + ' vs ' + countBefore);
