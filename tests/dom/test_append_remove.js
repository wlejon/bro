// Test appendChild, removeChild, insertBefore, replaceChild

const root = document.getElementById('root');
assert(root !== null, 'root element exists');

// appendChild
const a = document.createElement('div');
a.id = 'a';
root.appendChild(a);
assert(a.parentNode === root, 'appendChild sets parentNode');
assert(root.children.length === 1, 'parent has 1 child after appendChild');
assert(root.children[0] === a, 'child is at index 0');

// Append second child
const b = document.createElement('div');
b.id = 'b';
root.appendChild(b);
assert(root.children.length === 2, 'parent has 2 children');
assert(root.children[1] === b, 'second child at index 1');

// insertBefore
const c = document.createElement('div');
c.id = 'c';
root.insertBefore(c, b);
assert(root.children.length === 3, 'parent has 3 children after insertBefore');
assert(root.children[0] === a, 'a is still first');
assert(root.children[1] === c, 'c inserted before b');
assert(root.children[2] === b, 'b is now last');

// removeChild
root.removeChild(c);
assert(root.children.length === 2, 'parent has 2 children after removeChild');
assert(c.parentNode === null, 'removed child has null parentNode');
assert(root.children[0] === a, 'a still first after remove');
assert(root.children[1] === b, 'b now second after remove');

// replaceChild
const d = document.createElement('div');
d.id = 'd';
root.replaceChild(d, a);
assert(root.children.length === 2, 'still 2 children after replaceChild');
assert(root.children[0] === d, 'd replaced a');
assert(a.parentNode === null, 'replaced child has null parentNode');

// Cleanup
root.innerHTML = '';
assert(root.children.length === 0, 'innerHTML="" clears children');
