// Test addEventListener and removeEventListener

const root = document.getElementById('root');
root.innerHTML = '<div id="target" style="width:100px;height:50px;position:absolute;left:0;top:0;">target</div>';
flush();

const target = document.getElementById('target');
let count = 0;

function handler(e) { count++; }

// Add listener
target.addEventListener('click', handler);
click(50, 25);
assert(count === 1, 'handler called once');

// Second click
click(50, 25);
assert(count === 2, 'handler called twice');

// Remove listener
target.removeEventListener('click', handler);
click(50, 25);
assert(count === 2, 'handler not called after removeEventListener');

// Multiple listeners on same event
let a = 0, b = 0;
function handlerA() { a++; }
function handlerB() { b++; }

target.addEventListener('click', handlerA);
target.addEventListener('click', handlerB);
click(50, 25);
assert(a === 1 && b === 1, 'both handlers called');

// Remove only one
target.removeEventListener('click', handlerA);
click(50, 25);
assert(a === 1, 'removed handler not called');
assert(b === 2, 'remaining handler still called');

// Cleanup
root.innerHTML = '';
