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

// Cleanup
root.innerHTML = '';
