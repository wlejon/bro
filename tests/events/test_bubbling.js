// Test event bubbling

const root = document.getElementById('root');
root.innerHTML = '<div id="outer" style="width:200px;height:200px;position:absolute;left:0;top:0;"><div id="inner" style="width:100px;height:100px;">inner</div></div>';
flush();

const outer = document.getElementById('outer');
const inner = document.getElementById('inner');

const log = [];

outer.addEventListener('click', (e) => {
    log.push('outer:' + e.target.id);
});

inner.addEventListener('click', (e) => {
    log.push('inner:' + e.target.id);
});

// Click on inner element - should bubble to outer
click(50, 50);

assert(log.length === 2, 'both handlers called, got ' + log.length);
assert(log[0] === 'inner:inner', 'inner fires first: ' + log[0]);
assert(log[1] === 'outer:inner', 'outer fires second with same target: ' + log[1]);

// Cleanup
root.innerHTML = '';
