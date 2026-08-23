/**
 * =============================================================================
 * bro.gizmo — engine-level 3D transform gizmo
 * =============================================================================
 *
 * Renders standardized translate / rotate / scale handles inside the scene's
 * 3D pipeline, hit-tests against them, and drives the default drag
 * interaction. Apps hook in via callbacks, position/orientation read the
 * pivot each frame, translate/rotate/scale receive the per-frame delta to
 * apply to whatever the app considers "selected".
 *
 * Handles:
 *   - translate: 3 axis arrows + 3 plane quads (XY / YZ / XZ)
 *   - rotate:    3 axis rings + an outer screen-facing ring
 *   - scale:     3 axis boxes + center uniform cube
 *
 * A plane quad drags in the plane it spans, keeping the grabbed point under
 * the cursor and leaving the third axis untouched. The screen-facing ring
 * rotates about the view axis, the one rotation the three world rings
 * cannot express comfortably. There is still no translate center (free-move)
 * handle, so `hovered` is 'center' in scale mode only.
 *
 * `hovered` values: 'x' | 'y' | 'z' | 'xy' | 'yz' | 'xz' | 'view' | 'center'
 * | null. Plane and view handles report through the same `translate` and
 * `rotate` callbacks as the axis handles, an app that only reads the delta
 * needs no changes to support them.
 *
 * Visuals stay at ~80px regardless of camera zoom or projection mode
 * (configurable via configure({size})). Depth test is disabled so handles
 * always win over scene geometry, grabbable even when inside meshes.
 *
 * Picking ranks candidates by distance to the cursor, not by depth, so the
 * handle you are pointing at is the one you get even where several overlap
 * near the pivot.
 *
 * Rotation is measured entirely in screen pixels, against the camera as it
 * stood when the handle was grabbed. So an app that moves the camera in
 * response to `rotate` (a view gizmo that orbits) does not feed its own motion
 * back in and change the drag's speed mid-gesture.
 *
 * The grabbed ring's tangent sets the DIRECTION the cursor must travel to turn
 * it forwards; the rate is fixed for the whole drag at the ring's face-on pixel
 * radius. So equal travel turns any ring by the same amount however
 * foreshortened it looks, a ring behaves like a wheel (its near and far edges
 * turn opposite ways), and motion across the ring rather than along it
 * correctly rotates nothing. On a steeply foreshortened ring the grabbed point
 * no longer stays exactly under the cursor, the usual trade for a rate that
 * does not swing.
 *
 * @example
 *   // Basic display and mode
 *   bro.gizmo.show();
 *   bro.gizmo.setMode("translate");
 *   bro.gizmo.setPosition(0, 5, 0);
 *
 * @example
 *   // Attach interactive transform handlers
 *   let target = { x: 0, y: 0, z: 0 };
 *   bro.gizmo.attach({
 *     position: () => [target.x, target.y, target.z],
 *     translate: (dx, dy, dz) => {
 *       target.x += dx; target.y += dy; target.z += dz;
 *     },
 *   });
 *
 * @example
 *   // Configure appearance
 *   bro.gizmo.configure({
 *     size: 100,
 *     colors: { x: "#ff0000", y: "#00ff00", z: "#0000ff", hover: "#ffff00", active: "#ffffff" },
 *   });
 */

// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * RGBA / Hex color configuration for gizmo axes and interaction states.
 * @typedef {Object} GizmoColors
 * @property {string} [x] -  Hex color string for X axis handle (e.g. "#e74c3c").
 * @property {string} [y] -  Hex color string for Y axis handle (e.g. "#27ae60").
 * @property {string} [z] -  Hex color string for Z axis handle (e.g. "#3498db").
 * @property {string} [hover] -  Hex color string for hovered handle (e.g. "#ffd166").
 * @property {string} [active] -  Hex color string for active/dragged handle (e.g. "#ffffff").
 */

/**
 * Configuration options for gizmo sizing, styling, and rendering.
 * @typedef {Object} GizmoConfig
 * @property {number} [size] -  Target pixel size on screen (default ~80).
 * @property {GizmoColors} [colors] -  Axis and state color palette.
 * @property {number} [emissive] -  Emissive intensity multiplier when idle.
 * @property {number} [emissiveHover] -  Emissive intensity multiplier when hovered.
 * @property {boolean} [alwaysOnTop] -  Whether handles render over all scene geometry without depth testing.
 */

/**
 * Event and transform handlers for gizmo interaction.
 * @typedef {Object} GizmoHandlers
 * @property {Function} [position] -  Callback returning pivot position [x, y, z] or {x, y, z}.
 * @property {Function} [orientation] -  Callback returning orientation quaternion [x, y, z, w].
 * @property {Function} [beginDrag] -  Callback fired when a drag gesture begins.
 * @property {Function} [translate] -  Callback receiving translation delta (dx, dy, dz) in world space.
 * @property {Function} [rotate] -  Callback receiving rotation delta quaternion (qx, qy, qz, qw) in world space.
 * @property {Function} [scale] -  Callback receiving scale delta factor (sx, sy, sz).
 * @property {Function} [endDrag] -  Callback fired when a drag gesture ends.
 * @property {Function} [hoverChange] -  Callback fired when the hovered axis handle changes.
 */

// ── Namespaces ───────────────────────────────────────────────────────────────

/**
 * Engine-level 3D transform gizmo namespace.
 */
/**
 * Whether the gizmo is currently visible.
 * @readonly
 * @type {boolean}
 */
bro.gizmo.visible;

/**
 * Whether a transform handle is currently being dragged.
 * @readonly
 * @type {boolean}
 */
bro.gizmo.dragging;

/**
 * The handle currently under the cursor ('x' | 'y' | 'z' | 'xy' | 'yz' | 'xz' | 'view' | 'center' | null).
 * @readonly
 * @type {string|null}
 */
bro.gizmo.hovered;

/**
 * Make the transform gizmo visible (no-op if already visible).
 */
bro.gizmo.show = function() {};

/**
 * Hide the transform gizmo and disable hit-testing and picking.
 */
bro.gizmo.hide = function() {};

/**
 * Set the transform interaction mode ('translate' | 'rotate' | 'scale').
 *
 * @param {string} mode - Transform mode string
 */
bro.gizmo.setMode = function(mode) {};

/**
 * Set the coordinate reference space ('world' | 'local').
 *
 * @param {string} space - Coordinate space string
 */
bro.gizmo.setSpace = function(space) {};

/**
 * Set explicit pivot position in world space (overridden if position callback is attached).
 *
 * @param {number} x - World X coordinate
 * @param {number} y - World Y coordinate
 * @param {number} z - World Z coordinate
 */
bro.gizmo.setPosition = function(x, y, z) {};

/**
 * Set explicit handle orientation quaternion in local space.
 *
 * @param {number} x - Quaternion X component
 * @param {number} y - Quaternion Y component
 * @param {number} z - Quaternion Z component
 * @param {number} w - Quaternion W component
 */
bro.gizmo.setOrientation = function(x, y, z, w) {};

/**
 * Configure gizmo visuals, size, and handle colors.
 *
 * @param {GizmoConfig} config - Gizmo appearance and dimension configuration
 */
bro.gizmo.configure = function(config) {};

/**
 * Subscribe to engine-driven transform interactions and attach event callbacks.
 *
 * @param {GizmoHandlers} handlers - Interaction and delta callback handlers
 */
bro.gizmo.attach = function(handlers) {};

/**
 * Detach all interaction handlers, clear callbacks, and hide the gizmo.
 */
bro.gizmo.detach = function() {};

