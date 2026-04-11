// Test innerHTML getter and setter

const root = document.getElementById('root');

// Set innerHTML
root.innerHTML = '<div id="a">hello</div>';
flush();
const a = document.getElementById('a');
assert(a !== null, 'innerHTML creates element with id');
assert(a.textContent === 'hello', 'innerHTML creates text content');

// innerHTML getter produces HTML
const html = root.innerHTML;
assert(html.indexOf('hello') !== -1, 'innerHTML getter contains text');
assert(html.indexOf('<div') !== -1, 'innerHTML getter contains tag');

// Nested HTML
root.innerHTML = '<ul><li>one</li><li>two</li><li>three</li></ul>';
flush();
const lis = root.querySelectorAll('li');
assert(lis.length === 3, 'nested innerHTML creates correct structure');
assert(lis[0].textContent === 'one', 'first li text');
assert(lis[2].textContent === 'three', 'third li text');

// Setting innerHTML clears previous children
root.innerHTML = '<span>only</span>';
flush();
assert(root.children.length === 1, 'innerHTML replaces children');
assert(root.children[0].tagName === 'SPAN', 'new child is span');

// Empty innerHTML clears everything
root.innerHTML = '';
assert(root.children.length === 0, 'empty innerHTML clears all children');
assert(root.textContent === '', 'empty innerHTML clears text');
