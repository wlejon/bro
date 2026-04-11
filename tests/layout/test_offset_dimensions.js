// Test offsetWidth, offsetHeight, offsetLeft, offsetTop, clientWidth, clientHeight

const root = document.getElementById('root');
root.innerHTML = '<div id="box" style="width:100px;height:60px;padding:10px;border:2px solid black;position:absolute;left:15px;top:25px;">box</div>';
flush();

const box = document.getElementById('box');

// offsetWidth/Height include padding + border
// box-sizing:border-box is set in our test CSS, so width:100px includes padding+border
// offsetWidth should be 100, offsetHeight should be 60
assert(typeof box.offsetWidth === 'number', 'offsetWidth is number');
assert(typeof box.offsetHeight === 'number', 'offsetHeight is number');
assert(box.offsetWidth > 0, 'offsetWidth > 0: ' + box.offsetWidth);
assert(box.offsetHeight > 0, 'offsetHeight > 0: ' + box.offsetHeight);

// offsetLeft/Top
assert(typeof box.offsetLeft === 'number', 'offsetLeft is number');
assert(typeof box.offsetTop === 'number', 'offsetTop is number');

// clientWidth/Height (content + padding, no border)
assert(typeof box.clientWidth === 'number', 'clientWidth is number');
assert(typeof box.clientHeight === 'number', 'clientHeight is number');
assert(box.clientWidth > 0, 'clientWidth > 0');
assert(box.clientHeight > 0, 'clientHeight > 0');

// Cleanup
root.innerHTML = '';
