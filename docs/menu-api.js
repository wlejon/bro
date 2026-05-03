/**
 * bro.menu — Standard application menu bar
 *
 * A horizontal menu bar rendered by the engine's system panel
 * (system/menu.html). Hidden by default — tooling apps that want a
 * menu (project manager, editors, IDE-like tools) call bro.menu.show()
 * at startup. bro-headless suppresses the bar regardless of state so
 * screenshots match a clean no-menu viewport.
 *
 * Actions prefixed with "__system." are engine-handled:
 *   __system.quit         — close the app (sets running=false)
 *   __system.preferences  — open the settings overlay (Graphics/Audio/Input)
 *
 * All other action IDs dispatch to handlers registered via bro.menu.on(id, fn).
 * The ESC key is no longer reserved by the engine — apps are free to use it.
 *
 * Default menu (engine-built):
 *   File  — Quit (Ctrl+Q → __system.quit)
 *   Edit  — Preferences... (__system.preferences)
 */

// ── Show / Hide ──────────────────────────────────────────────────────────────

bro.menu.show();
bro.menu.hide();
bro.menu.visible;   // read-only boolean


// ── Set the tree (replaces any existing menus) ──────────────────────────────

bro.menu.set([
    { id: 'file', label: 'File', items: [
        { id: 'file.new',   label: 'New',         accel: 'Ctrl+N' },
        { id: 'file.open',  label: 'Open...',     accel: 'Ctrl+O' },
        { id: 'file.save',  label: 'Save',        accel: 'Ctrl+S', enabled: false },
        { separator: true },
        { id: '__system.quit', label: 'Quit',     accel: 'Ctrl+Q' },
    ]},
    { id: 'edit', label: 'Edit', items: [
        { id: 'edit.undo',   label: 'Undo', accel: 'Ctrl+Z' },
        { id: 'edit.redo',   label: 'Redo', accel: 'Ctrl+Y' },
        { separator: true },
        { id: '__system.preferences', label: 'Preferences...' },
    ]},
    { id: 'view', label: 'View', items: [
        { id: 'view.grid', label: 'Show Grid', checked: true },
    ]},
]);


// ── Mutate the tree ──────────────────────────────────────────────────────────

// Add an item. parentId = '' (empty) appends a new root; otherwise appends
// into that submenu. index is optional (negative = append).
bro.menu.addItem('file', { id: 'file.recent', label: 'Recent', items: [
    { id: 'recent.1', label: 'project1.json' },
    { id: 'recent.2', label: 'project2.json' },
]}, /* index */ 2);

// Update any mutable property of an item by id.
bro.menu.updateItem('file.save', { enabled: true });
bro.menu.updateItem('view.grid', { checked: false });
bro.menu.updateItem('file.recent', { hidden: true });

// Remove an item (anywhere in the tree).
bro.menu.removeItem('recent.2');


// ── Register action handlers ────────────────────────────────────────────────

bro.menu.on('file.open', async () => {
    const path = await showOpenFileDialog({ filters: 'JSON|json' });
    if (path) loadProject(path);
});

bro.menu.on('file.save', () => { /* ... */ });
bro.menu.on('view.grid', () => {
    gridVisible = !gridVisible;
    bro.menu.updateItem('view.grid', { checked: gridVisible });
});


// ── Item shape ──────────────────────────────────────────────────────────────
//
// {
//   id:        string         — unique id, or '__system.*' for engine actions
//   label:     string         — display text
//   accel:     string         — shortcut hint, display-only (e.g. 'Ctrl+S')
//   separator: boolean        — draw as a horizontal divider (ignores everything else)
//   enabled:   boolean        — default true; false greys out and ignores clicks
//   hidden:    boolean        — default false; true hides the item entirely
//   checked:   boolean        — default false; true shows a checkmark
//   items:     Array<Item>    — children (for submenus / roots)
// }
//
// Only root items (top-level) show on the menu bar. Each root's `items` are
// rendered as a dropdown when the root is clicked. Submenus nested deeper
// than one level are not rendered by the default menu.html.


// ── Opt out ─────────────────────────────────────────────────────────────────

// For fullscreen apps (FPS, games, kiosk):
bro.menu.hide();


// ── Replace the look entirely ───────────────────────────────────────────────
//
// Drop your own renderer at <your-app>/system/menu.html. The engine
// prefers app-local system panels over the global ones — same mechanism
// as perf.html / settings/*.html overrides. Your replacement has the same
// __bro bridge: __bro.getMenu() returns the current tree, __bro.menuClick(id)
// dispatches actions, window.__onMenuChanged is called on tree mutations.
