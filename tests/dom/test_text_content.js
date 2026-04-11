// Test textContent getter and setter

const root = document.getElementById('root');

// Set textContent on element
const div = document.createElement('div');
div.textContent = 'hello world';
root.appendChild(div);
assert(div.textContent === 'hello world', 'textContent getter');
assert(div.childNodes.length === 1, 'textContent creates one text node');

// textContent strips child elements
div.innerHTML = '<span>a</span> <span>b</span>';
assert(div.textContent.indexOf('a') !== -1, 'textContent includes nested text');
assert(div.textContent.indexOf('b') !== -1, 'textContent includes all nested text');

// Setting textContent replaces all children
div.innerHTML = '<span>x</span><span>y</span>';
assert(div.children.length === 2, 'has 2 child elements before');
div.textContent = 'replaced';
assert(div.children.length === 0, 'textContent removes child elements');
assert(div.textContent === 'replaced', 'textContent set correctly');

// createTextNode
const text = document.createTextNode('direct');
root.appendChild(text);
assert(text.nodeType === 3, 'text node type is 3');
assert(text.textContent === 'direct', 'text node textContent');

// Cleanup
root.innerHTML = '';
