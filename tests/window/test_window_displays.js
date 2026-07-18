// bro.window.getDisplays / moveToDisplay + window.screen.
// Headless enumerates the REAL displays (the hidden window sits on one), so
// only shapes/invariants are asserted — never machine-specific values.
// window.screen is pinned to the hidden window's size in headless (same
// determinism policy as devicePixelRatio).

// ---- display enumeration --------------------------------------------------
const displays = bro.window.getDisplays();
assert(Array.isArray(displays), 'getDisplays returns array');
assert(displays.length >= 1, 'at least one display, got ' + displays.length);

for (const d of displays) {
    assert(typeof d.id === 'number' && d.id > 0, 'display id is positive number');
    assert(typeof d.name === 'string', 'display name is string');
    for (const rect of [d.bounds, d.workArea]) {
        assert(typeof rect === 'object' && rect !== null, 'bounds/workArea object');
        assert(Number.isInteger(rect.x) && Number.isInteger(rect.y), 'rect x/y integers');
        assert(Number.isInteger(rect.width) && rect.width > 0, 'rect width positive');
        assert(Number.isInteger(rect.height) && rect.height > 0, 'rect height positive');
    }
    assert(d.workArea.width <= d.bounds.width, 'work area fits bounds (w)');
    assert(d.workArea.height <= d.bounds.height, 'work area fits bounds (h)');
    assert(typeof d.refreshRate === 'number' && d.refreshRate >= 0, 'refreshRate number');
    assert(typeof d.contentScale === 'number' && d.contentScale > 0, 'contentScale positive');
    assert(typeof d.isPrimary === 'boolean', 'isPrimary boolean');
    assert(typeof d.isCurrent === 'boolean', 'isCurrent boolean');
}
assert(displays.filter(d => d.isPrimary).length === 1, 'exactly one primary display');
assert(displays.filter(d => d.isCurrent).length === 1, 'exactly one current display');

// ---- moveToDisplay no-ops (returns false) in headless ---------------------
assert(bro.window.moveToDisplay(displays[0].id) === false,
       'moveToDisplay no-ops headless');
assert(bro.window.moveToDisplay(999999) === false, 'unknown display id rejected');

// ---- window.screen --------------------------------------------------------
assert(typeof screen === 'object', 'window.screen exists');
// Headless pins screen to the hidden window's size; width tracks the
// viewport width exactly (both come from graphics.width).
assert(screen.width === innerWidth, 'headless screen.width pinned to window width: '
       + screen.width + ' vs ' + innerWidth);
assert(screen.height >= innerHeight && screen.height > 0,
       'headless screen.height pinned to window height');
assert(screen.availWidth === screen.width, 'headless availWidth === width');
assert(screen.availHeight === screen.height, 'headless availHeight === height');
assert(screen.colorDepth === 24, 'colorDepth 24');
assert(screen.pixelDepth === 24, 'pixelDepth 24');

console.log('displays + screen OK');
