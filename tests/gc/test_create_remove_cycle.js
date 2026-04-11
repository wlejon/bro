// Test rapid create/append/remove cycles don't crash or leak detectably

const root = document.getElementById('root');

for (let cycle = 0; cycle < 10; cycle++) {
    // Build a subtree
    const container = document.createElement('div');
    for (let i = 0; i < 20; i++) {
        const child = document.createElement('span');
        child.textContent = 'item ' + i;
        child.addEventListener('click', () => {});  // add listener to exercise GC paths
        container.appendChild(child);
    }
    root.appendChild(container);
    flush();

    assert(root.children.length === 1, 'cycle ' + cycle + ': one container');
    assert(container.children.length === 20, 'cycle ' + cycle + ': 20 children');

    // Tear it all down
    root.innerHTML = '';
    flush();
    assert(root.children.length === 0, 'cycle ' + cycle + ': cleared');
}

// Final sanity check
const div = document.createElement('div');
div.textContent = 'still alive';
root.appendChild(div);
assert(root.textContent === 'still alive', 'DOM still functional after cycles');

// Cleanup
root.innerHTML = '';
