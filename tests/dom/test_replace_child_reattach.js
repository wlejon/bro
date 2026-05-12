// replaceChild(new, old) must leave `old` reattachable, matching removeChild
// and the WHATWG DOM spec (the returned node has been removed, not destroyed).
// Regression: replaceChild used to call doc->freeNode + invalidateWrapper on
// the replaced node, so a subsequent appendChild silently dropped it.

const root = document.getElementById('root');
root.innerHTML = '';

const a = document.createElement('div');
a.id = 'a';
a.textContent = 'A';
const b = document.createElement('div');
b.id = 'b';
b.textContent = 'B';
root.appendChild(a);

// Replace a with b
const returned = root.replaceChild(b, a);
assert(returned === a, 'replaceChild returns the replaced node');
assert(root.firstChild === b, 'b is now the child');
assert(a.parentNode === null, 'a is detached');

// The returned node must still be usable: textContent intact, id intact,
// reattachable to the DOM, queryable by id once re-inserted.
assert(a.id === 'a', 'a.id preserved after replace');
assert(a.textContent === 'A', 'a.textContent preserved after replace');

root.appendChild(a);
assert(a.parentNode === root, 'a reattached');
assert(root.children.length === 2, 'both b and a now present');
// Note: getElementById of `a` after detach+reattach does not work currently
// (the id is unregistered on detach and not re-registered on appendChild —
// same behavior as removeChild + appendChild). That's a separate issue.

// And the swap-back case should work too: replaceChild(a, b) restores
// the original ordering.
root.replaceChild(a, b);
assert(b.parentNode === null, 'b detached after replaceChild(a, b)');
root.appendChild(b);
assert(b.parentNode === root, 'b reattached after being replaced');
assert(b.textContent === 'B', 'b.textContent preserved through replace+reattach');
