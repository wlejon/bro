// Test cloneNode (shallow and deep)

const root = document.getElementById('root');

// Setup
const orig = document.createElement('div');
orig.id = 'original';
orig.className = 'cls';
orig.setAttribute('data-x', '1');
const child = document.createElement('span');
child.textContent = 'child text';
orig.appendChild(child);
root.appendChild(orig);

// Shallow clone
const shallow = orig.cloneNode(false);
assert(shallow !== orig, 'clone is a different object');
assert(shallow.tagName === 'DIV', 'clone has same tag');
assert(shallow.getAttribute('data-x') === '1', 'clone has attributes');
assert(shallow.children.length === 0, 'shallow clone has no children');

// Deep clone
const deep = orig.cloneNode(true);
assert(deep.children.length === 1, 'deep clone has children');
assert(deep.children[0].tagName === 'SPAN', 'deep clone child is span');
assert(deep.children[0].textContent === 'child text', 'deep clone preserves text');
assert(deep.children[0] !== child, 'deep clone children are different objects');

// Clone does not have a parent
assert(shallow.parentNode === null, 'clone has no parent');
assert(deep.parentNode === null, 'deep clone has no parent');

// Cleanup
root.innerHTML = '';
