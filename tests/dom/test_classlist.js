// Test classList API

const el = document.createElement('div');
document.getElementById('root').appendChild(el);

// Initially empty
assert(el.classList.length === 0, 'classList initially empty');
assert(!el.classList.contains('foo'), 'does not contain foo');

// add
el.classList.add('foo');
assert(el.classList.contains('foo'), 'contains foo after add');
assert(el.classList.length === 1, 'length is 1');

// add multiple
el.classList.add('bar');
assert(el.classList.contains('bar'), 'contains bar');
assert(el.classList.length === 2, 'length is 2');

// add duplicate (should not duplicate)
el.classList.add('foo');
assert(el.classList.length === 2, 'add duplicate does not increase length');

// remove
el.classList.remove('foo');
assert(!el.classList.contains('foo'), 'foo removed');
assert(el.classList.length === 1, 'length is 1 after remove');

// toggle
const result1 = el.classList.toggle('baz');
assert(result1 === true, 'toggle returns true when adding');
assert(el.classList.contains('baz'), 'baz added by toggle');

const result2 = el.classList.toggle('baz');
assert(result2 === false, 'toggle returns false when removing');
assert(!el.classList.contains('baz'), 'baz removed by toggle');

// className reflects classList
el.className = '';
el.classList.add('x');
el.classList.add('y');
assert(el.className.indexOf('x') !== -1, 'className contains x');
assert(el.className.indexOf('y') !== -1, 'className contains y');

// Cleanup
document.getElementById('root').innerHTML = '';
