// Test document.createElement and basic element properties

// Create element
const div = document.createElement('div');
assert(div !== null, 'createElement returns an element');
assert(div.tagName === 'DIV', 'tagName is uppercase');
assert(div.nodeType === 1, 'nodeType is 1 (ELEMENT_NODE)');
assert(div.nodeName === 'DIV', 'nodeName matches tagName');

// Create different elements
const span = document.createElement('span');
assert(span.tagName === 'SPAN', 'span tagName');

const p = document.createElement('p');
assert(p.tagName === 'P', 'p tagName');

// New element has no parent
assert(div.parentNode === null, 'new element has no parentNode');
assert(div.parentElement === null, 'new element has no parentElement');

// New element has no children
assert(div.childNodes.length === 0, 'new element has no childNodes');
assert(div.children.length === 0, 'new element has no children');

// textContent is empty
assert(div.textContent === '', 'new element textContent is empty');

// innerHTML is empty
assert(div.innerHTML === '', 'new element innerHTML is empty');
