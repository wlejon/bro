// Test getBoundingClientRect

const root = document.getElementById('root');
root.innerHTML = '<div id="box" style="width:150px;height:80px;position:absolute;left:20px;top:30px;">box</div>';
flush();

const box = document.getElementById('box');
const rect = box.getBoundingClientRect();

assert(rect !== null && rect !== undefined, 'getBoundingClientRect returns object');
assert(typeof rect.x === 'number', 'rect.x is number');
assert(typeof rect.y === 'number', 'rect.y is number');
assert(typeof rect.width === 'number', 'rect.width is number');
assert(typeof rect.height === 'number', 'rect.height is number');

// Check dimensions (should match style)
assert(Math.abs(rect.width - 150) < 1, 'width is ~150, got ' + rect.width);
assert(Math.abs(rect.height - 80) < 1, 'height is ~80, got ' + rect.height);

// Check position
assert(Math.abs(rect.left - 20) < 1, 'left is ~20, got ' + rect.left);
assert(Math.abs(rect.top - 30) < 1, 'top is ~30, got ' + rect.top);

// right and bottom
assert(Math.abs(rect.right - 170) < 1, 'right is ~170, got ' + rect.right);
assert(Math.abs(rect.bottom - 110) < 1, 'bottom is ~110, got ' + rect.bottom);

// Cleanup
root.innerHTML = '';
