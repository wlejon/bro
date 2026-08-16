// Test stopPropagation

const root = document.getElementById('root');
root.innerHTML = '<div id="outer" style="width:200px;height:200px;position:absolute;left:0;top:0;"><div id="inner" style="width:100px;height:100px;">inner</div></div>';
flush();

const outer = document.getElementById('outer');
const inner = document.getElementById('inner');

let outerCalled = false;
let innerCalled = false;

inner.addEventListener('click', (e) => {
    innerCalled = true;
    e.stopPropagation();
});

outer.addEventListener('click', (e) => {
    outerCalled = true;
});

click(50, 50);

assert(innerCalled, 'inner handler called');
assert(!outerCalled, 'outer handler NOT called due to stopPropagation');

// stopPropagation() must NOT cut short the other listeners on the same
// element (DOM §2.9) — only stopImmediatePropagation() does. UI toolkits
// routinely register a second listener on a control whose first one stops
// propagation, and used to lose it entirely.
root.innerHTML = '<div id="o2" style="width:200px;height:200px;position:absolute;left:0;top:0;"><div id="i2" style="width:100px;height:100px;">inner</div></div>';
flush();

const o2 = document.getElementById('o2');
const i2 = document.getElementById('i2');
let order = [];

i2.addEventListener('click', (e) => { order.push('first'); e.stopPropagation(); });
i2.addEventListener('click', () => order.push('second'));
o2.addEventListener('click', () => order.push('outer'));

click(50, 50);
assert(order.join(',') === 'first,second',
       'stopPropagation keeps same-element listeners, drops the ancestor: ' + order.join(','));

// stopImmediatePropagation() does cut the rest of this element's listeners.
root.innerHTML = '<div id="o3" style="width:200px;height:200px;position:absolute;left:0;top:0;"><div id="i3" style="width:100px;height:100px;">inner</div></div>';
flush();

const o3 = document.getElementById('o3');
const i3 = document.getElementById('i3');
order = [];

i3.addEventListener('click', (e) => { order.push('first'); e.stopImmediatePropagation(); });
i3.addEventListener('click', () => order.push('second'));
o3.addEventListener('click', () => order.push('outer'));

click(50, 50);
assert(order.join(',') === 'first',
       'stopImmediatePropagation drops the rest: ' + order.join(','));

// Cleanup
root.innerHTML = '';
