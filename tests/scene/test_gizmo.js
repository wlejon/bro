// Test bro.gizmo — show/hide, modes, configure, attach.
// Exercises src/engine/gizmo.cpp and src/js/gizmo_bindings.cpp.

assert(typeof bro === 'object', 'bro');
assert(typeof bro.gizmo === 'object', 'bro.gizmo');

// =========================================================================
// show / hide
// =========================================================================
bro.gizmo.show();
bro.gizmo.hide();
bro.gizmo.show();

// =========================================================================
// setMode (translate / rotate / scale)
// =========================================================================
bro.gizmo.setMode('translate');
bro.gizmo.setMode('rotate');
bro.gizmo.setMode('scale');

// =========================================================================
// setSpace
// =========================================================================
bro.gizmo.setSpace('world');
bro.gizmo.setSpace('local');

// =========================================================================
// setPosition
// =========================================================================
bro.gizmo.setPosition(1, 2, 3);
bro.gizmo.setPosition(0, 0, 0);

// =========================================================================
// setOrientation
// =========================================================================
bro.gizmo.setOrientation(0, 0, 0, 1);
bro.gizmo.setOrientation(0, 0.707, 0, 0.707);

// =========================================================================
// configure
// =========================================================================
bro.gizmo.configure({ size: 80 });
bro.gizmo.configure({
    size: 100,
    colors: { x: '#ff0000', y: '#00ff00', z: '#0000ff' },
    emissive: 0.5,
    emissiveHover: 1.5,
    alwaysOnTop: true,
});

// =========================================================================
// attach with handlers
// =========================================================================
let selected = { x: 0, y: 0, z: 0 };
let beganCount = 0, endedCount = 0, hoverCount = 0;

bro.gizmo.attach({
    position:    () => [selected.x, selected.y, selected.z],
    orientation: () => [0, 0, 0, 1],
    beginDrag:   () => beganCount++,
    translate:   (dx, dy, dz) => { selected.x += dx; selected.y += dy; selected.z += dz; },
    rotate:      (qx, qy, qz, qw) => { /* noop */ },
    scale:       (sx, sy, sz) => { /* noop */ },
    endDrag:     () => endedCount++,
    hoverChange: () => hoverCount++,
});

// position accepting object too
bro.gizmo.attach({
    position: () => ({ x: 0, y: 5, z: 0 }),
});

// =========================================================================
// detach / reset
// =========================================================================
if (typeof bro.gizmo.detach === 'function') {
    bro.gizmo.detach();
}

// Hovered (read-only)
const h = bro.gizmo.hovered;
// May be null in headless without GPU input pass
assert(h === null || typeof h === 'string' || h === undefined, 'hovered is null/string/undef');

bro.gizmo.hide();
