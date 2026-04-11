// Test getAttribute, setAttribute, hasAttribute, removeAttribute

const el = document.createElement('div');

// Initially no attributes
assert(!el.hasAttribute('data-x'), 'no data-x initially');
assert(el.getAttribute('data-x') === null || el.getAttribute('data-x') === '', 'getAttribute returns null/empty for missing');

// setAttribute
el.setAttribute('data-x', 'hello');
assert(el.hasAttribute('data-x'), 'hasAttribute after set');
assert(el.getAttribute('data-x') === 'hello', 'getAttribute returns value');

// Overwrite
el.setAttribute('data-x', 'world');
assert(el.getAttribute('data-x') === 'world', 'setAttribute overwrites');

// removeAttribute
el.removeAttribute('data-x');
assert(!el.hasAttribute('data-x'), 'hasAttribute false after remove');

// id property
el.id = 'myid';
assert(el.id === 'myid', 'id getter');
assert(el.getAttribute('id') === 'myid', 'id reflected in getAttribute');

// className property
el.className = 'foo bar';
assert(el.className === 'foo bar', 'className getter');
assert(el.getAttribute('class') === 'foo bar', 'className reflected in getAttribute');

// Boolean-ish attributes
el.setAttribute('hidden', '');
assert(el.hasAttribute('hidden'), 'empty string attribute exists');
assert(el.getAttribute('hidden') === '', 'empty string attribute value');
