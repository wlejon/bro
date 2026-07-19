/**
 * bro.menu — Standard application menu bar
 *
 * A horizontal menu bar rendered by the engine's system panel
 * (system/menu.html). Hidden by default — tooling apps that want a
 * menu (project manager, editors, IDE-like tools) call bro.menu.show()
 * at startup. The bar behaves identically in bro-headless: showing it
 * reserves the same top inset and it appears in screenshots, so headless
 * runs exercise the exact viewport geometry the windowed app gets.
 *
 * Actions prefixed with "__system." are engine-handled:
 *   __system.quit         — close the app (sets running=false)
 *   __system.preferences  — open the settings overlay (Graphics/Audio/Input)
 *   __system.inspector    — toggle the DOM inspector panel; item is auto-checked when open
 *   __system.togglePerf   — toggle the perf HUD overlay (same as F8)
 *
 * Only the inspector item syncs its checkmark from the engine side. The
 * __system.togglePerf item is re-checked inside its own menu handler, so
 * toggling the HUD with F8 leaves the menu item's checkmark stale — mirror it
 * yourself with updateItem() if you offer both paths.
 *
 * All other action IDs dispatch to handlers registered via bro.menu.on(id, fn).
 * The ESC key is no longer reserved by the engine — apps are free to use it.
 *
 * bro.menu lives on the primary app realm only. Code in an <iframe> or in a
 * secondary window (bro.window.open) has no bro.menu, and a secondary window
 * has no menu bar of its own — the bar belongs to the main window.
 *
 * Default menu (engine-built):
 *   File  — Quit (Ctrl+Q → __system.quit)
 *   Edit  — Preferences... (__system.preferences)
 *   View  — Inspector (__system.inspector)
 */

// ── Show / Hide ──────────────────────────────────────────────────────────────

bro.menu.show();
bro.menu.hide();
bro.menu.visible;   // read-only boolean (a non-enumerable accessor — it does
                    // not show up in Object.keys(bro.menu) or a spread copy)


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

// addItem / updateItem / removeItem each return a boolean: true if the target
// was found and the tree changed, false otherwise (e.g. an unknown id). They
// do not throw on a miss — check the return value.
//
// Add an item. parentId = '' (empty) appends a new root; otherwise appends
// into that submenu. index is optional (negative = append). Any non-string
// parentId or id is coerced to '' rather than rejected, so addItem(null, item)
// quietly appends a new ROOT instead of erroring.
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

// showOpenFileDialog is synchronous and takes a filter STRING (not an options
// object — a non-string first argument is silently ignored, showing all
// files). It returns an array of paths, empty on cancel — so test .length,
// never truthiness. See docs/dialogs-api.js.
bro.menu.on('file.open', () => {
    const files = showOpenFileDialog('JSON|json');
    if (files.length) loadProject(files[0]);
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
// __bro bridge: __bro.menu.getTree() returns the current tree, __bro.menu.click(id)
// dispatches actions, window.__onMenuChanged is called on tree mutations.
// Full system-panel authoring reference: docs/system-panels.md.
