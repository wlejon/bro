// =============================================================================
// bro.gizmo — engine-level 3D transform gizmo.
//
// Renders standardized translate / rotate / scale handles inside the scene's
// 3D pipeline, hit-tests against them, and drives the default drag
// interaction. Apps hook in via callbacks — position/orientation read the
// pivot each frame, translate/rotate/scale receive the per-frame delta to
// apply to whatever the app considers "selected".
//
// Handles:
//   - translate: 3 axis arrows + 3 plane quads (XY / YZ / XZ)
//   - rotate:    3 axis rings + an outer screen-facing ring
//   - scale:     3 axis boxes + center uniform cube
//
// A plane quad drags in the plane it spans, keeping the grabbed point under
// the cursor and leaving the third axis untouched. The screen-facing ring
// rotates about the view axis — the one rotation the three world rings
// cannot express comfortably. There is still no translate center (free-move)
// handle, so `hovered` is 'center' in scale mode only.
//
// `hovered` values: 'x' | 'y' | 'z' | 'xy' | 'yz' | 'xz' | 'view' | 'center'
// | null. Plane and view handles report through the same `translate` and
// `rotate` callbacks as the axis handles — an app that only reads the delta
// needs no changes to support them.
//
// Visuals stay at ~80px regardless of camera zoom or projection mode
// (configurable via configure({size})). Depth test is disabled so handles
// always win over scene geometry — grabbable even when inside meshes.
//
// Picking ranks candidates by distance to the cursor, not by depth, so the
// handle you are pointing at is the one you get even where several overlap
// near the pivot.
//
// Rotation is measured entirely in screen pixels, against the camera as it
// stood when the handle was grabbed — so an app that moves the camera in
// response to `rotate` (a view gizmo that orbits) does not feed its own motion
// back in and change the drag's speed mid-gesture.
//
// The grabbed ring's tangent sets the DIRECTION the cursor must travel to turn
// it forwards; the rate is fixed for the whole drag at the ring's face-on pixel
// radius. So equal travel turns any ring by the same amount however
// foreshortened it looks, a ring behaves like a wheel (its near and far edges
// turn opposite ways), and motion across the ring rather than along it
// correctly rotates nothing. On a steeply foreshortened ring the grabbed point
// no longer stays exactly under the cursor — the usual trade for a rate that
// does not swing.
// =============================================================================

bro.gizmo.show();                     // make visible (no-op if already shown)
bro.gizmo.hide();                     // hide + disables picking / input

bro.gizmo.setMode('translate');       // 'translate' | 'rotate' | 'scale'
bro.gizmo.setSpace('world');          // 'world' | 'local'

// Explicit pivot (ignored when a `position` callback is attached).
bro.gizmo.setPosition(x, y, z);

// Used when setSpace('local') — rotates each handle to match the target's
// world-space orientation. Ignored in world mode.
bro.gizmo.setOrientation(qx, qy, qz, qw);

// Visuals. Only `size` and colors.x/y/z/hover have any visible effect —
// `emissive`, `emissiveHover`, `alwaysOnTop` and `colors.active` are parsed
// and stored but never read by the renderer. (Handles are always unlit and
// always drawn on top regardless of alwaysOnTop; a dragged axis keeps its
// hover color.)
bro.gizmo.configure({
    size: 80,                         // target pixel height on screen
    colors: {
        x: '#e74c3c', y: '#27ae60', z: '#3498db',
        hover: '#ffd166',
        active: '#ffffff',            // stored, unused
    },
    emissive: 0.55,                   // stored, unused
    emissiveHover: 1.4,               // stored, unused
    alwaysOnTop: true,                // stored, unused (always on top)
});

// -----------------------------------------------------------------------------
// attach(handlers) — subscribe to the engine-driven interaction.
// All handlers are optional; any subset works. Keys:
//
//   position():    [x,y,z]   - called each frame for the pivot (attach to a
//                               moving target without per-frame setPosition).
//                              Also accepts {x,y,z}.
//   orientation(): [x,y,z,w] - quaternion; only called when space='local'.
//   beginDrag():   void      - fired when the user grabs a handle.
//   translate(dx,dy,dz):     - translate delta in world space, per-frame.
//                              Apply to your target's position.
//   rotate(qx,qy,qz,qw):     - quaternion rotation delta in world space.
//   scale(sx,sy,sz):         - per-axis multiplicative factor (e.g. {1.02,1,1}
//                              each frame while dragging +X scale).
//   endDrag():    void       - fired on mouseUp.
//   hoverChange(): void      - hovered axis changed. Read bro.gizmo.hovered.
//
// Calling attach() also shows the gizmo implicitly.
// -----------------------------------------------------------------------------
let selected = { x: 0, y: 0, z: 0, qx: 0, qy: 0, qz: 0, qw: 1 };
bro.gizmo.attach({
    position:    () => [selected.x, selected.y, selected.z],
    orientation: () => [selected.qx, selected.qy, selected.qz, selected.qw],
    beginDrag:   () => { /* snapshot for undo */ },
    translate:   (dx, dy, dz) => {
        selected.x += dx; selected.y += dy; selected.z += dz;
    },
    rotate: (qx, qy, qz, qw) => {
        // quaternion multiply: new = delta * current
        const a = { x: qx, y: qy, z: qz, w: qw };
        const b = { x: selected.qx, y: selected.qy, z: selected.qz, w: selected.qw };
        selected.qx = a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y;
        selected.qy = a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x;
        selected.qz = a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w;
        selected.qw = a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z;
    },
    scale: (sx, sy, sz) => {
        /* apply per-axis scale factor to selected */
    },
    endDrag: () => { /* commit undo */ },
});

// Detach and hide. Clears all callbacks.
bro.gizmo.detach();

// Read-only state.
bro.gizmo.visible;                   // bool
bro.gizmo.dragging;                  // bool
bro.gizmo.hovered;                   // 'x'|'y'|'z'|'xy'|'yz'|'xz'|'view'|'center'|null
