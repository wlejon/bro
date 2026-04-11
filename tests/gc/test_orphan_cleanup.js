// Test that removed elements don't cause crashes when GC runs

const root = document.getElementById('root');

// Create and remove many elements to stress GC
for (let i = 0; i < 100; i++) {
    const div = document.createElement('div');
    div.id = 'temp-' + i;
    div.textContent = 'content ' + i;
    root.appendChild(div);
}
flush();

assert(root.children.length === 100, 'created 100 children');

// Remove all children
root.innerHTML = '';
flush();

assert(root.children.length === 0, 'all children removed');

// The old elements should not be findable
const found = document.getElementById('temp-0');
assert(found === null, 'removed element not findable by id');
