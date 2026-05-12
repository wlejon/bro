// Test bro.menu — show/hide, set/addItem/updateItem/removeItem, on().
// Exercises src/js/menu_bindings.cpp and src/engine/menu_bar.cpp.

assert(typeof bro.menu === 'object', 'bro.menu exists');

// =========================================================================
// show / hide / visible
// =========================================================================
bro.menu.hide();
assert(bro.menu.visible === false, 'visible false after hide');

bro.menu.show();
assert(bro.menu.visible === true, 'visible true after show');

bro.menu.hide();

// =========================================================================
// set replaces the tree
// =========================================================================
bro.menu.set([
    { id: 'file', label: 'File', items: [
        { id: 'file.new', label: 'New', accel: 'Ctrl+N' },
        { id: 'file.open', label: 'Open', accel: 'Ctrl+O' },
        { separator: true },
        { id: 'file.save', label: 'Save', enabled: false },
        { id: '__system.quit', label: 'Quit' },
    ]},
    { id: 'edit', label: 'Edit', items: [
        { id: 'edit.undo', label: 'Undo' },
        { id: 'edit.redo', label: 'Redo' },
    ]},
]);

// =========================================================================
// addItem (append to existing submenu)
// =========================================================================
bro.menu.addItem('file', {
    id: 'file.recent', label: 'Recent', items: [
        { id: 'recent.1', label: 'project1.json' },
        { id: 'recent.2', label: 'project2.json' },
    ]
}, 1);

// addItem at root
bro.menu.addItem('', { id: 'view', label: 'View', items: [
    { id: 'view.grid', label: 'Grid', checked: true },
]});

// =========================================================================
// updateItem
// =========================================================================
bro.menu.updateItem('file.save', { enabled: true });
bro.menu.updateItem('view.grid', { checked: false });
bro.menu.updateItem('file.recent', { hidden: true });
bro.menu.updateItem('file.recent', { hidden: false });

// Update label
bro.menu.updateItem('edit.undo', { label: 'Undo!' });

// =========================================================================
// removeItem
// =========================================================================
bro.menu.removeItem('recent.2');
bro.menu.removeItem('edit.redo');

// =========================================================================
// on() handler registration
// =========================================================================
let opened = 0;
bro.menu.on('file.open', () => opened++);
bro.menu.on('file.save', () => { /* noop */ });

// =========================================================================
// getMenu (if exposed for inspection)
// =========================================================================
if (typeof bro.menu.getMenu === 'function') {
    const tree = bro.menu.getMenu();
    assert(Array.isArray(tree), 'getMenu returns array');
}

// =========================================================================
// Empty / clear
// =========================================================================
bro.menu.set([]);

// Smoke: pass odd inputs without crashing
bro.menu.removeItem('nonexistent');
bro.menu.updateItem('nonexistent', { enabled: false });
