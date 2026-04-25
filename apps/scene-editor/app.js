// =============================================================================
// Scene editor — entrypoint.
//
// Orchestrates orbit camera, tool modes, input, and the VCB. Per-object state
// (mesh, BVH, face groups, inference geo, render + edges nodes) lives on
// Primitive; the PrimitiveRegistry tracks every editable object and powers
// pickAt + multi-primitive inference. The app itself is mostly tool modes,
// event routing, and UI glue.
// =============================================================================

const canvas     = document.getElementById('canvas');
const scene      = canvas.getContext('scene');
const pickInfo   = document.getElementById('pick-info');
const toolName   = document.getElementById('tool-name');
const snapInfo   = document.getElementById('snap-info');
const snapMarker = document.getElementById('snap-marker');

// --- Registry of editable primitives ---------------------------------------
//
// The app works entirely through the registry — no hard-coded per-object
// bindings. The default scene seeds one box (same as the spike) so existing
// tests and the on-open experience stay identical.

const registry = new SceneRegistry({ scene });

// Default scene factory — called at startup and from project.new() to reset
// to a blank slate. One box at the origin matches the pre-project behavior.
function setupDefaultScene() {
    registry.clear();
    registry.create({
        type: 'box',
        name: 'Box',
        color: '#b8bcc2',
        params: { sx: 1, sy: 1, sz: 1 },
    });
}
setupDefaultScene();

// --- Highlight overlay ------------------------------------------------------
//
// Shared across primitives (only one drag / selection at a time). The
// highlight node lives outside any primitive — it's pure UI and rebuilt on
// every pick.

const HIGHLIGHT_EPS = 0.002;
let highlightNode      = null;
let highlightPrimitive = null;
// Last setHighlightTriangles inputs — used by refreshHighlight() to rebuild
// the overlay against an updated positions buffer (e.g. during a move-gizmo
// drag, where the primitive's preview positions change every frame but the
// picked tri indices and face normal are stable).
let highlightTris      = null;
let highlightNormal    = null;

function clearHighlight() {
    if (highlightNode) { highlightNode.destroy(); highlightNode = null; }
    highlightPrimitive = null;
    highlightTris = null;
    highlightNormal = null;
}

// Copy a Primitive's composed world-transform onto an arbitrary scene node.
// Used for overlays (highlight, edges) that carry the primitive's local-
// space geometry but need to render under the same transform.
function applyPrimitiveTransformToNode(node, prim) {
    if (!node || !prim) return;
    const w = prim.getWorldMatrix();
    const dec = Mat4Lib.decomposeTRS(w);
    const eul = Mat4Lib.quatToEuler(dec.rotation);
    node.x = dec.translation[0]; node.y = dec.translation[1]; node.z = dec.translation[2];
    node.rotationX = eul[0]; node.rotationY = eul[1]; node.rotationZ = eul[2];
    node.scaleX = dec.scale[0]; node.scaleY = dec.scale[1]; node.scaleZ = dec.scale[2];
}

// Build a highlight scene node from triangle indices into `primitive`. The
// overlay is nudged along the group normal to avoid z-fighting. During
// push/pull, the working (not-yet-committed) buffers can be passed as
// overrides so the highlight tracks the drag.
function setHighlightTriangles(primitive, triIndices, normal, positionsSrc, indicesSrc) {
    clearHighlight();
    if (!primitive || !triIndices || triIndices.length === 0) return;
    const posSrc = positionsSrc || primitive.positions;
    const idxSrc = indicesSrc   || primitive.indices;
    const nx = normal[0] * HIGHLIGHT_EPS;
    const ny = normal[1] * HIGHLIGHT_EPS;
    const nz = normal[2] * HIGHLIGHT_EPS;
    const n = triIndices.length;
    const positions = new Float32Array(n * 9);
    const normals   = new Float32Array(n * 9);
    const indices   = new Uint32Array(n * 3);
    for (let i = 0; i < n; i++) {
        const t = triIndices[i];
        for (let k = 0; k < 3; k++) {
            const src = idxSrc[t * 3 + k] * 3;
            const dst = (i * 3 + k) * 3;
            positions[dst + 0] = posSrc[src + 0] + nx;
            positions[dst + 1] = posSrc[src + 1] + ny;
            positions[dst + 2] = posSrc[src + 2] + nz;
            normals[dst + 0] = normal[0];
            normals[dst + 1] = normal[1];
            normals[dst + 2] = normal[2];
        }
        indices[i * 3 + 0] = i * 3 + 0;
        indices[i * 3 + 1] = i * 3 + 1;
        indices[i * 3 + 2] = i * 3 + 2;
    }
    highlightNode = scene.createMesh({
        positions, normals, indices,
        color: '#ffa502',
        unlit: true,
        name: 'highlight',
    });
    applyPrimitiveTransformToNode(highlightNode, primitive);
    highlightPrimitive = primitive;
    highlightTris      = triIndices;
    highlightNormal    = normal;
}

// Refresh the highlight. With `positionsSrc` supplied, rebuild the triangles
// from the given local buffers (push/pull surgery path — the tri set shifts
// around as the mesh mutates). Without, just re-sync the overlay node's
// transform to the primitive's current TRS — used by move/rotate/scale, which
// don't change the local triangle set, only the composed world transform.
function refreshHighlight(positionsSrc, indicesSrc) {
    if (!highlightPrimitive || !highlightTris || !highlightNormal) return;
    if (positionsSrc) {
        setHighlightTriangles(
            highlightPrimitive, highlightTris, highlightNormal,
            positionsSrc, indicesSrc);
    } else if (highlightNode) {
        applyPrimitiveTransformToNode(highlightNode, highlightPrimitive);
    }
}

function setHighlightTriangle(primitive, triIndex, normal) {
    setHighlightTriangles(primitive, [triIndex], normal);
}

function setHighlightFaceGroup(primitive, groupIdx) {
    if (!primitive) { clearHighlight(); return; }
    const g = primitive.faceGroups.groups[groupIdx];
    if (!g) { clearHighlight(); return; }
    setHighlightTriangles(primitive, g.tris, g.normal);
}

// --- Tool state (CURRENT_TOOL_DECL) ----------------------------------------
//
// Declared early because applyCamera() fires during script init and reaches
// updateGizmoForActive(), which needs currentTool + GIZMO_TOOLS.

let currentTool = 'select';

// Which tools drive the engine gizmo, and which mode they map to. 'select'
// and 'pushpull' deliberately hide the gizmo so picking doesn't compete
// with handle hits.
const GIZMO_TOOLS = { move: 'translate', rotate: 'rotate', scale: 'scale' };

// --- Ground grid + XYZ axes -------------------------------------------------
//
// Static visual aid; not a primitive, just a single mesh sitting outside the
// registry so it never participates in picking or snapping.

const sceneAxesData = SceneAxes.buildSceneAxes();
const sceneAxesNode = scene.createMesh({
    positions: sceneAxesData.positions,
    normals:   sceneAxesData.normals,
    colors:    sceneAxesData.colors,
    indices:   sceneAxesData.indices,
    name: 'scene-axes',
});

// --- Undo / redo history ---------------------------------------------------
//
// Pure-JS command stack (apps/lib/history.js). Every mutating user action
// is recorded as one entry: outliner add/delete/rename, move/rotate/scale
// commit, push-pull commit. Visibility and selection aren't recorded —
// they're view state, matching SketchUp.
//
// Mesh-state edits (move/rotate/scale/pushpull) all resolve to
//   prev = captureMesh(prim); ... ; next = captureMesh(prim);
//   history.record(label, () => applyMesh(prim, next),
//                         () => applyMesh(prim, prev));
// applyMesh routes through Primitive.updateGeometry so BVH / face groups /
// inference / edges rebuild from the restored buffers.

const history = new History({ limit: 200 });

// Lightweight TRS capture for pure-transform history (move/rotate/scale).
// Works on any SceneObject — groups, primitives, component instances.
function captureTransform(obj) {
    return {
        translation: obj.translation.slice(),
        rotation:    obj.rotation.slice(),
        scale:       obj.scale.slice(),
    };
}
function applyTransform(obj, snap) {
    obj.setTRS(snap.translation, snap.rotation, snap.scale);
}
function transformChanged(prev, obj) {
    if (!prev) return false;
    for (let i = 0; i < 3; i++) {
        if (prev.translation[i] !== obj.translation[i]) return true;
        if (prev.scale[i]       !== obj.scale[i])       return true;
    }
    for (let i = 0; i < 4; i++) {
        if (prev.rotation[i] !== obj.rotation[i]) return true;
    }
    return false;
}

function captureMesh(prim) {
    return {
        positions:   new Float32Array(prim.positions),
        indices:     new Uint32Array(prim.indices),
        normals:     prim.normals ? new Float32Array(prim.normals) : null,
        translation: prim.translation.slice(),
        rotation:    prim.rotation.slice(),
        scale:       prim.scale.slice(),
    };
}
function applyMesh(prim, snap) {
    // Only rebuild topology if buffer identity changed. Move/rotate/scale
    // snapshots have buffers === the previous step's buffers (we reference-
    // clone Float32/Uint32 copies on capture), so we always go through
    // updateGeometry. updateGeometry rebuilds BVH + face groups + edit mesh
    // + inference; the transform is applied separately to the node.
    prim.updateGeometry(snap.positions, snap.indices, snap.normals);
    prim.setTRS(snap.translation, snap.rotation, snap.scale);
}
// "Did anything visible change?" — buffers OR transform. Skips recording no-
// op drags (zero-distance move, identity rotate/scale, zero push-pull).
function meshChanged(prev, prim) {
    if (!prev) return false;
    if (prev.positions.length !== prim.positions.length) return true;
    if (prev.indices.length   !== prim.indices.length)   return true;
    const a = prev.positions, b = prim.positions;
    for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return true;
    for (let i = 0; i < 3; i++) {
        if (prev.translation[i] !== prim.translation[i]) return true;
        if (prev.scale[i]       !== prim.scale[i])       return true;
    }
    for (let i = 0; i < 4; i++) {
        if (prev.rotation[i] !== prim.rotation[i]) return true;
    }
    return false;
}

// --- Project: save / load / new (apps/lib/project.js) ----------------------
//
// Serialize the registry to a JSON-friendly state. Raw mesh buffers ride as
// plain arrays — JSON-safe and fast enough for the scene sizes we target.
// Binary sidecars under `my-scene.bro/assets/` are a future optimization
// once files grow big enough for embedded arrays to feel slow.

// Schema 2: full SceneObject tree (TRS + kind + children); replaces schema 1's
// flat primitive list. No legacy migration path — this branch isn't shipped
// yet.
const PROJECT_SCHEMA = 2;

function _serializeNode(n) {
    const base = {
        id:          n.id,
        name:        n.name,
        visible:     n.visible,
        kind:        n.kind,
        translation: Array.from(n.translation),
        rotation:    Array.from(n.rotation),
        scale:       Array.from(n.scale),
    };
    if (n.kind === 'primitive') {
        base.color     = n.color;
        base.positions = Array.from(n.positions);
        base.indices   = Array.from(n.indices);
        base.normals   = n.normals ? Array.from(n.normals) : null;
    } else if (n.kind === 'component-instance') {
        base.definitionId = n.definition ? n.definition.id : null;
    } else {
        base.children = n.children.map(_serializeNode);
    }
    return base;
}

function _serializeComponent(def) {
    return {
        id:   def.id,
        name: def.name,
        // The component's root is a Group whose children form the shared
        // subtree; serialize as a synthetic node tree.
        root: _serializeNode(def.root),
    };
}

function serializeScene() {
    const tree = { children: registry.root.children.map(_serializeNode) };
    // Also project a flat `primitives` array — depth-first over all
    // primitive leaves in the tree. Convenience for tests + tooling that
    // iterates primitives without walking the hierarchy. The canonical
    // structure is still `tree`.
    const flat = [];
    (function walk(node) {
        if (node.kind === 'primitive') flat.push(node);
        else if (node.children) node.children.forEach(walk);
    })(tree);
    return {
        components: registry.components.map(_serializeComponent),
        tree,
        primitives: flat,
        activeId:   registry.active ? registry.active.id : null,
        nextAddX:   _outlinerNextAddX,
    };
}

function _deserializeNodeInto(parent, data, componentsById) {
    if (data.kind === 'primitive') {
        // Tree is loaded top-down; attach directly to the already-constructed
        // parent instead of routing through restoreFromSnapshot's id-based
        // parent resolution (descendants' ids aren't known yet at top).
        const seed = SceneRegistry.buildMeshFromSpec(
            { type: 'box', params: { sx: 1, sy: 1, sz: 1 } });
        const prim = new Primitive({
            id:          data.id,
            name:        data.name,
            color:       data.color,
            scene:       registry.scene,
            mesh:        seed,
            translation: data.translation,
            rotation:    data.rotation,
            scale:       data.scale,
        });
        prim.updateGeometry(
            new Float32Array(data.positions),
            new Uint32Array(data.indices),
            data.normals ? new Float32Array(data.normals) : null);
        prim.setVisible(data.visible !== false);
        parent.addChild(prim);
        if (data.id >= registry._nextId) registry._nextId = data.id + 1;
        return prim;
    }
    if (data.kind === 'component-instance') {
        const def = componentsById.get(data.definitionId);
        if (!def) return null;
        const inst = new ComponentInstance({
            id:          data.id,
            name:        data.name,
            scene:       registry.scene,
            definition:  def,
            translation: data.translation,
            rotation:    data.rotation,
            scale:       data.scale,
            visible:     data.visible,
        });
        parent.addChild(inst);
        if (data.id >= registry._nextId) registry._nextId = data.id + 1;
        return inst;
    }
    // Group (or the synthetic root wrapper).
    const g = new Group({
        id:          data.id,
        name:        data.name,
        visible:     data.visible,
        translation: data.translation,
        rotation:    data.rotation,
        scale:       data.scale,
    });
    parent.addChild(g);
    if (data.id >= registry._nextId) registry._nextId = data.id + 1;
    if (data.children) {
        for (const c of data.children) _deserializeNodeInto(g, c, componentsById);
    }
    return g;
}

function deserializeScene(data) {
    registry.clear();
    clearHighlight();
    registry._nextId = 1;

    // Components first — instances reference them by id.
    const componentsById = new Map();
    if (data.components) {
        for (const cd of data.components) {
            const def = new ComponentDefinition({ id: cd.id, name: cd.name });
            if (cd.id >= registry._nextId) registry._nextId = cd.id + 1;
            // Populate definition.root's children from serialized children.
            if (cd.root && cd.root.children) {
                for (const c of cd.root.children) _deserializeNodeInto(def.root, c, componentsById);
            }
            registry.components.push(def);
            componentsById.set(def.id, def);
        }
    }

    if (data.tree && data.tree.children) {
        for (const c of data.tree.children) _deserializeNodeInto(registry.root, c, componentsById);
    }

    if (data.activeId != null) {
        const t = registry.getById(data.activeId);
        if (t) registry.active = t;
    }
    if (typeof data.nextAddX === 'number') _outlinerNextAddX = data.nextAddX;
    MeasureBox.clearLastOp(measureBoxState);
    MeasureBox.clear(measureBoxState);
    MeasureBox.setActive(measureBoxState, false);
    renderMeasureBox();
    updateGizmoForActive();
}

const proj = new Project({
    app:         'scene-editor',
    schema:      PROJECT_SCHEMA,
    serialize:   serializeScene,
    deserialize: deserializeScene,
    onNew:       setupDefaultScene,
    history,
    // No prompt in headless/MVP — the app stays silent on dirty.
});

// --- Translate gizmo (engine-rendered — bro.gizmo.*) -----------------------
//
// Pivot + drag + hit-testing all live in the engine; this file only tells
// the gizmo *where* to sit (the active primitive's bbox centroid) and what
// to do with the per-frame world delta (forward to MoveTool.applyDelta for
// preview + commit via the same pipeline the Move tool uses).

const GIZMO_TARGET_PX = 80;
bro.gizmo.configure({ size: GIZMO_TARGET_PX });

// --- Camera -----------------------------------------------------------------

const cam = Camera.createOrbit({ target: [0, 0, 0], dist: 4, fov: 45 });

function applyCamera() {
    scene.setCamera(Camera.orbitViewOpts(cam, canvas));
    // Scale of engine gizmo tracks camera distance automatically; we only
    // need to keep its pivot in sync with the active primitive.
    updateGizmoForActive();
}
applyCamera();

// --- Lighting --------------------------------------------------------------
//
// Editor look: evenly lit from all angles, no shadows. High ambient gives
// every face a readable base color regardless of orientation; two soft
// directionals (top-front key + opposite fill) add just enough shading to
// read shape without making any face go dark. Linear tonemap keeps authored
// vertex colors faithful (no ACES desaturation on the red/green/blue axes).
//
// Primitives and the ground/axes use the default lit shader so they
// participate in depth occlusion. UI affordances (highlight, tool previews,
// snap spheres, edge wireframes) stay `unlit:true` to read as crisp overlays.

scene.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 2.2 });
scene.setAmbient([0.55, 0.56, 0.58]);
scene.createLight({
    type:      'directional',
    direction: [-0.4, -1.0, -0.3],
    color:     [1.0, 0.98, 0.94],
    intensity: 0.7,
    name:      'editor-key',
});
scene.createLight({
    type:      'directional',
    direction: [0.5, -0.3, 0.6],
    color:     [0.88, 0.92, 1.0],
    intensity: 0.4,
    name:      'editor-fill',
});

// --- Screen → world ray -----------------------------------------------------

// Transform a world-space ray into a primitive's LOCAL frame. Direction is
// re-normalized so the local ray is unit-length; any non-uniform scale in
// the primitive's transform is absorbed into a `worldPerLocal` factor so
// callers can map local t-values back to world distance if they need to.
function worldRayToLocal(ray, prim) {
    const inv = prim.getWorldInverse();
    const o = Mat4Lib.transformPoint(inv, ray.origin);
    const d = Mat4Lib.transformDir(inv, ray.dir);
    const L = Math.hypot(d[0], d[1], d[2]) || 1;
    return {
        origin: o,
        dir:    [d[0]/L, d[1]/L, d[2]/L],
        worldPerLocal: L,
    };
}

function screenToRay(px, py) {
    const opts = Camera.orbitViewOpts(cam, canvas);
    const w = canvas.clientWidth || canvas.width;
    const h = canvas.clientHeight || canvas.height;
    const nx = (2 * px / w) - 1;
    const ny = 1 - (2 * py / h);
    const tanHalf = Math.tan(opts.fov * Math.PI / 180 * 0.5);
    // orbitViewOpts intentionally omits `aspect` so the engine can derive it
    // from the canvas FBO. For ray casting we still need it; use the same
    // canvas dimensions the engine uses for the projection.
    const aspect  = w / Math.max(1, h);

    const fx = opts.target[0] - opts.position[0];
    const fy = opts.target[1] - opts.position[1];
    const fz = opts.target[2] - opts.position[2];
    const fl = Math.hypot(fx, fy, fz) || 1;
    const f  = [fx / fl, fy / fl, fz / fl];

    const up = opts.up;
    let rx = f[1] * up[2] - f[2] * up[1];
    let ry = f[2] * up[0] - f[0] * up[2];
    let rz = f[0] * up[1] - f[1] * up[0];
    const rl = Math.hypot(rx, ry, rz) || 1;
    rx /= rl; ry /= rl; rz /= rl;

    const ux = ry * f[2] - rz * f[1];
    const uy = rz * f[0] - rx * f[2];
    const uz = rx * f[1] - ry * f[0];

    const sx = nx * aspect * tanHalf;
    const sy = ny * tanHalf;
    let dx = f[0] + sx * rx + sy * ux;
    let dy = f[1] + sx * ry + sy * uy;
    let dz = f[2] + sx * rz + sy * uz;
    const dl = Math.hypot(dx, dy, dz) || 1;
    dx /= dl; dy /= dl; dz /= dl;

    return {
        origin: [opts.position[0], opts.position[1], opts.position[2]],
        dir:    [dx, dy, dz],
    };
}

// pickAt returns { primitive, hit } or null. `excludeId` skips a primitive
// (used by Move so the cursor can target geometry behind the moving object).
function pickAt(px, py, excludeId) {
    const ray = screenToRay(px, py);
    return registry.pickAt(ray.origin, ray.dir,
        excludeId != null ? { excludeId } : null);
}

// Camera forward direction (unit). Used by Move to set the drag plane to a
// camera-facing plane through the grab pivot, so mouse motion maps 1:1 to
// world distance at the pivot's depth.
function cameraForward() {
    const opts = Camera.orbitViewOpts(cam, canvas);
    const fx = opts.target[0] - opts.position[0];
    const fy = opts.target[1] - opts.position[1];
    const fz = opts.target[2] - opts.position[2];
    const fl = Math.hypot(fx, fy, fz) || 1;
    return [fx / fl, fy / fl, fz / fl];
}

function formatHit(pick) {
    if (!pick) return 'no pick';
    const hit = pick.hit;
    const p = hit.position;
    const n = hit.normal;
    return `[${pick.primitive.name}] tri ${hit.triangleIndex}  dist ${hit.distance.toFixed(3)}\n` +
           `pos [${p[0].toFixed(2)}, ${p[1].toFixed(2)}, ${p[2].toFixed(2)}]\n` +
           `nrm [${n[0].toFixed(2)}, ${n[1].toFixed(2)}, ${n[2].toFixed(2)}]`;
}

// --- Tool modes -------------------------------------------------------------

// Tool state is declared near the top (see CURRENT_TOOL_DECL) so
// applyCamera() → updateGizmoForActive() can read it during script init.

function setTool(t) {
    if (t === currentTool) return;
    if (gizmoDrag.active)       cancelGizmoDrag();
    if (pushpull.active)        cancelPushPull();
    if (moveToolState.active)   cancelMove();
    if (rotateToolState && rotateToolState.active) RotateTool.cancel(rotateToolState);
    if (scaleToolState  && scaleToolState.active)  ScaleTool.cancel(scaleToolState);
    if (rectangleToolState.active) cancelRectangle();
    if (circleToolState.active) cancelCircle();
    if (lineToolState.active) cancelLine();
    if (arcToolState && ArcTool.active(arcToolState)) cancelArc();
    if (offsetToolState && OffsetTool.active(offsetToolState)) cancelOffset();
    if (tapeToolState.active) cancelTape();
    MeasureBox.clearLastOp(measureBoxState);
    MeasureBox.clear(measureBoxState);
    MeasureBox.setActive(measureBoxState, false);
    renderMeasureBox();
    currentTool = t;
    if (toolName) toolName.textContent = t;
    clearHighlight();
    showSnapMarker(null);
    pickInfo.textContent = 'no pick';
    // Toolbar visual state. Indexed loop — NodeList isn't iterable here.
    const btns = document.querySelectorAll('.tool-btn');
    for (let i = 0; i < btns.length; i++) {
        btns[i].classList.toggle('active', btns[i].getAttribute('data-tool') === t);
    }
    // Switch gizmo mode / visibility for the new tool.
    updateGizmoForActive();
}

// --- Push/Pull --------------------------------------------------------------
//
// SketchUp-style: never warps the cap rim. Each drag dup-extrudes the picked
// face along its normal, building bridge quads for every boundary edge:
//   - bridges coplanar with an adjacent face merge into that face group
//     (cap stays flat, side facets stretch — same observed behavior as
//     SketchUp's "follow the existing surface")
//   - non-coplanar bridges form new wall face groups
//   - if the pushed face was the entire mesh (sketch face), a back-face
//     copy at the original position closes the slab into a manifold
//
// The surgery composes EditMesh primitives (findFaceGroupBoundary,
// duplicateBoundary, addBridge, rematchTwins). Topology is rebuilt from the
// committed primitive on every applyPushPull tick — no incremental state
// between frames, so cancel/preview/commit all go through the same path.

// Closest-point parameter along the axis-line passing through `pivot` for a
// cursor ray. Returns t such that (pivot + t*axis) is the closest point on
// the axis-line to the ray. Returns 0 if the ray is parallel to the axis
// (harmlessly freezes the drag until the cursor moves off-axis).
function rayVsAxisDistance(ray, pivot, axis) {
    const wx = ray.origin[0] - pivot[0];
    const wy = ray.origin[1] - pivot[1];
    const wz = ray.origin[2] - pivot[2];
    const b = ray.dir[0] * axis[0] + ray.dir[1] * axis[1] + ray.dir[2] * axis[2];
    const dDot = ray.dir[0] * wx + ray.dir[1] * wy + ray.dir[2] * wz;
    const eDot = axis[0] * wx + axis[1] * wy + axis[2] * wz;
    const denom = 1 - b * b;
    if (denom < 1e-6) return 0;
    return (eDot - b * dDot) / denom;
}

const pushpull = {
    active: false,
    primitive: null,          // Primitive the drag is bound to
    groupIdx: -1,             // group of pushed face in the captured snapshot
    faceTriangles: null,      // tris in the pushed face group (snapshot indices)
    axis: [0, 0, 0],          // pushed face's outward normal
    pivot: [0, 0, 0],
    distance: 0,

    // Captured at beginPushPull, used as the surgery source on every frame.
    // (Mesh state, group assignment, normals — pristine pre-drag.)
    snapPositions: null,
    snapIndices:   null,
    snapNormals:   null,
    snapTriToGroup: null,
    snapFaceGroups: null,

    prevMesh: null,           // captureMesh(primitive) for cancel/undo

    // Cache of the most recent applyPushPull(t) result. commit consumes this.
    workingPositions: null,
    workingIndices:   null,
    workingNormals:   null,
    workingTriToGroup: null,
    workingFaceTris:   null,  // pushed face's tri indices in the surgery output
};

const BRIDGE_MERGE_TOL = 0.9995;   // cos(1.8°): coplanarity test for bridges

// Build a fresh push/pull surgery output for the snapshotted mesh, displacing
// the picked face by axis*t. Returns
//   { positions, indices, normals, triToGroup, faceTris }
// where faceTris is the moved face's triangle indices in the new buffer.
function surgicalPushPull(snap, gIdx, axis, t) {
    const offset = [axis[0] * t, axis[1] * t, axis[2] * t];
    const isSketchFace = snap.faceGroups.groups[gIdx].tris.length ===
                         snap.indices.length / 3;

    const em = EditMesh.fromMeshData(snap.positions, snap.indices, snap.triToGroup);
    const movedFaces = [];
    for (let i = 0; i < em.faces.length; i++) {
        if (em.faces[i].group === gIdx) movedFaces.push(em.faces[i]);
    }

    const baseGroupCount = snap.faceGroups.groups.length;
    let nextGroup = baseGroupCount;
    const newGroupNormals = [];   // indexed by (g - baseGroupCount)

    if (isSketchFace) {
        // --- Sketch face (2D polygon, no adjacencies): duplicate + bridge +
        // back face. The only way to make an open polygon into a closed slab.
        const { dupMap, oldBoundary } =
            EditMesh.duplicateBoundary(em, gIdx, offset);

        function assignBridgeGroup(rec, bridgeNormal) {
            if (rec.adjGroup >= 0 && rec.adjGroup < baseGroupCount) {
                const adjN = snap.faceGroups.groups[rec.adjGroup].normal;
                const dot = bridgeNormal[0]*adjN[0] +
                            bridgeNormal[1]*adjN[1] +
                            bridgeNormal[2]*adjN[2];
                if (dot > BRIDGE_MERGE_TOL) return rec.adjGroup;
            }
            const g = nextGroup++;
            newGroupNormals[g - baseGroupCount] = bridgeNormal.slice();
            return g;
        }
        for (const loop of oldBoundary) {
            for (const rec of loop) {
                const a = rec.oldA, b = rec.oldB;
                const ex = b.x - a.x, ey = b.y - a.y, ez = b.z - a.z;
                let bx = ey * offset[2] - ez * offset[1];
                let by = ez * offset[0] - ex * offset[2];
                let bz = ex * offset[1] - ey * offset[0];
                const bl = Math.hypot(bx, by, bz);
                if (bl > 1e-10) { bx /= bl; by /= bl; bz /= bl; }
                const grp = assignBridgeGroup(rec, [bx, by, bz]);
                EditMesh.addBridge(em, rec.oldA, rec.oldB, rec.newA, rec.newB, grp);
            }
        }
        // Back face at original positions (reversed winding) — closes the slab.
        const backGroup = nextGroup++;
        newGroupNormals[backGroup - baseGroupCount] = [
            -axis[0], -axis[1], -axis[2],
        ];
        for (const f of movedFaces) {
            const hes = EditMesh.faceHalfEdges(f);
            const origs = [];
            for (let k = 0; k < 3; k++) {
                const newV = hes[k].origin;
                let oldV = null;
                for (const [o, n] of dupMap.entries()) {
                    if (n === newV) { oldV = o; break; }
                }
                if (!oldV) throw new Error('back-face: vert not found in dupMap');
                origs.push(oldV);
            }
            const bf = { halfEdge: null, group: backGroup };
            em.faces.push(bf);
            const h0 = { origin: origs[0], twin: null, next: null, face: bf };
            const h1 = { origin: origs[2], twin: null, next: null, face: bf };
            const h2 = { origin: origs[1], twin: null, next: null, face: bf };
            em.halfEdges.push(h0, h1, h2);
            h0.next = h1; h1.next = h2; h2.next = h0;
            bf.halfEdge = h0;
            if (!origs[0].halfEdge) origs[0].halfEdge = h0;
            if (!origs[2].halfEdge) origs[2].halfEdge = h1;
            if (!origs[1].halfEdge) origs[1].halfEdge = h2;
        }
    } else {
        // --- Closed solid: translate in place. Adjacent faces deform via
        // shared vertex references — stay planar (cap adjacents keep their
        // plane; side adjacents tilt to a new plane through the moved edge).
        // No new verts, no new faces, no bridges — manifold is preserved.
        //
        // Walking by topology (one-ring via twin.next) rather than by
        // posKey: Float32 sin(2π) ≈ 1.7e-7 at cylinder seams, so seam-dup
        // verts aren't posKey-equal post-push. After collecting each topo
        // ring, we *snap* all members to one canonical new position — this
        // guarantees the serialized positions round-trip through posKey on
        // the next fromMeshData without breaking twin pairings.
        const vertsToMove = new Set();
        const snapGroups = [];
        function walkRing(startHE) {
            const ring = [];
            let cur = startHE;
            const seen = new Set();
            for (let iter = 0; iter < em.halfEdges.length + 1; iter++) {
                if (seen.has(cur)) break;
                seen.add(cur);
                if (!vertsToMove.has(cur.origin)) {
                    vertsToMove.add(cur.origin);
                    ring.push(cur.origin);
                }
                if (!cur.twin) break;
                cur = cur.twin.next;
            }
            if (ring.length > 1) snapGroups.push(ring);
        }
        // Interior verts of the pushed group move with no ring-snapping (they
        // have no seam dups in other groups by definition). Boundary verts
        // move via walkRing to sweep up seam dups in adjacent faces.
        const bdLoops = EditMesh.findFaceGroupBoundary(em, gIdx);
        const bdOrigins = new Set();
        for (const loop of bdLoops) {
            for (const he of loop) {
                bdOrigins.add(he.origin);
                bdOrigins.add(he.next.origin);
            }
        }
        for (const f of movedFaces) {
            const hes = EditMesh.faceHalfEdges(f);
            for (const he of hes) {
                if (bdOrigins.has(he.origin)) {
                    walkRing(he);
                } else {
                    vertsToMove.add(he.origin);
                }
            }
        }
        for (const v of vertsToMove) {
            v.x += offset[0];
            v.y += offset[1];
            v.z += offset[2];
        }
        // Snap each ring to a common position (use its first member's post-
        // move coords as the canonical value). Kills Float32 seam jitter.
        for (const ring of snapGroups) {
            const first = ring[0];
            for (let i = 1; i < ring.length; i++) {
                ring[i].x = first.x;
                ring[i].y = first.y;
                ring[i].z = first.z;
            }
        }
    }

    EditMesh.rematchTwins(em);

    const out = EditMesh.toMeshDataWithGroups(em);

    // Per-group flat normals recomputed from post-surgery geometry. Closed-
    // solid push may tilt adjacent side faces to a new plane, so the group
    // normals in `snap.faceGroups` are stale — take each group's normal
    // from its first triangle's cross product.
    const pos = out.positions;
    const idx = out.indices;
    const seenGroupNormal = new Map();   // groupId → [nx, ny, nz]
    const groupNormalForTri = new Float32Array(out.triToGroup.length * 3);
    for (let tt = 0; tt < out.triToGroup.length; tt++) {
        const g = out.triToGroup[tt];
        let gn = seenGroupNormal.get(g);
        if (!gn) {
            const i0 = idx[tt*3 + 0] * 3;
            const i1 = idx[tt*3 + 1] * 3;
            const i2 = idx[tt*3 + 2] * 3;
            const ax = pos[i1]   - pos[i0];
            const ay = pos[i1+1] - pos[i0+1];
            const az = pos[i1+2] - pos[i0+2];
            const bx = pos[i2]   - pos[i0];
            const by = pos[i2+1] - pos[i0+1];
            const bz = pos[i2+2] - pos[i0+2];
            let nx = ay*bz - az*by;
            let ny = az*bx - ax*bz;
            let nz = ax*by - ay*bx;
            const L = Math.hypot(nx, ny, nz);
            if (L > 1e-10) { nx /= L; ny /= L; nz /= L; }
            else { nx = 0; ny = 1; nz = 0; }
            gn = [nx, ny, nz];
            seenGroupNormal.set(g, gn);
        }
        groupNormalForTri[tt*3 + 0] = gn[0];
        groupNormalForTri[tt*3 + 1] = gn[1];
        groupNormalForTri[tt*3 + 2] = gn[2];
    }

    const normals = new Float32Array(out.positions.length);
    for (let tt = 0; tt < out.triToGroup.length; tt++) {
        const gn = seenGroupNormal.get(out.triToGroup[tt]);
        for (let k = 0; k < 3; k++) {
            const vi = idx[tt*3 + k];
            normals[vi*3 + 0] = gn[0];
            normals[vi*3 + 1] = gn[1];
            normals[vi*3 + 2] = gn[2];
        }
    }

    const movedFaceSet = new Set(movedFaces);
    const faceTris = [];
    for (let i = 0; i < em.faces.length; i++) {
        if (movedFaceSet.has(em.faces[i])) faceTris.push(i);
    }

    return {
        positions: out.positions,
        indices:   out.indices,
        normals,
        triToGroup: out.triToGroup,
        faceTris,
    };
}

// Begin push/pull. Snapshots the primitive's current mesh + face groups so
// every applyPushPull tick rebuilds from the same source — no incremental
// state between frames. Axis and pivot are stored in the primitive's LOCAL
// space (surgery mutates local positions); each frame converts the world
// ray / snap position into local before running the distance math.
function beginPushPull(primitive, hit) {
    if (!primitive) return;
    const gIdx = primitive.faceGroups.triToGroup[hit.triangleIndex];
    const g = primitive.faceGroups.groups[gIdx];

    pushpull.active = true;
    pushpull.primitive = primitive;
    pushpull.prevMesh = captureMesh(primitive);
    pushpull.groupIdx = gIdx;
    // g.normal is local. hit._localPosition is the raycast hit expressed
    // in primitive-local space (populated by Primitive.raycastWorld).
    pushpull.axis = g.normal.slice();
    pushpull.pivot = hit._localPosition
        ? hit._localPosition.slice()
        : primitive.worldToLocalPoint(hit.position);
    pushpull.distance = 0;
    pushpull.faceTriangles = g.tris.slice();

    pushpull.snapPositions  = new Float32Array(primitive.positions);
    pushpull.snapIndices    = new Uint32Array(primitive.indices);
    pushpull.snapNormals    = primitive.normals
        ? new Float32Array(primitive.normals) : null;
    pushpull.snapTriToGroup = new Int32Array(primitive.faceGroups.triToGroup);
    pushpull.snapFaceGroups = primitive.faceGroups;

    pushpull.workingPositions = null;
    pushpull.workingIndices   = null;
    pushpull.workingNormals   = null;
    pushpull.workingTriToGroup = null;
    pushpull.workingFaceTris  = null;

    // Highlight overlay is rendered through the primitive's scene node, so
    // it already inherits the primitive's transform — use LOCAL axis +
    // LOCAL positions.
    setHighlightTriangles(primitive, pushpull.faceTriangles, pushpull.axis);
    MeasureBox.clearLastOp(measureBoxState);
    MeasureBox.clear(measureBoxState);
    MeasureBox.setActive(measureBoxState, true);
    renderMeasureBox();
    // Seed at t=0 so a click-release without drag doesn't leave the working
    // buffers null at commit time.
    applyPushPull(0);
}

function applyPushPull(t) {
    if (!pushpull.active) return;
    const prim = pushpull.primitive;
    pushpull.distance = t;

    const snap = {
        positions:  pushpull.snapPositions,
        indices:    pushpull.snapIndices,
        normals:    pushpull.snapNormals,
        triToGroup: pushpull.snapTriToGroup,
        faceGroups: pushpull.snapFaceGroups,
    };
    const out = surgicalPushPull(snap, pushpull.groupIdx, pushpull.axis, t);
    pushpull.workingPositions  = out.positions;
    pushpull.workingIndices    = out.indices;
    pushpull.workingNormals    = out.normals;
    pushpull.workingTriToGroup = out.triToGroup;
    pushpull.workingFaceTris   = out.faceTris;

    prim.previewMesh(out.positions, out.indices, out.normals);
    setHighlightTriangles(prim, out.faceTris, pushpull.axis,
                          out.positions, out.indices);

    pickInfo.textContent = `push/pull  ${t.toFixed(3)}`;
}

function commitPushPull() {
    if (!pushpull.active) return;
    const prim = pushpull.primitive;
    const prevMesh = pushpull.prevMesh;
    const distance = pushpull.distance;

    // Zero distance: no topology change (surgicalPushPull at t=0 yields the
    // snapshot mesh extended with zero-length bridges + duplicated boundary
    // verts — degenerate). Revert and bail.
    if (distance === 0) {
        prim.revertMesh();
        pickInfo.textContent = 'push/pull cancelled (zero distance)';
        clearPushPull();
        clearHighlight();
        MeasureBox.clearLastOp(measureBoxState);
        MeasureBox.clear(measureBoxState);
        MeasureBox.setActive(measureBoxState, false);
        renderMeasureBox();
        return;
    }
    // lastOp is stored in WORLD space (the redo handler receives a world
    // reference normal + centroid and asks the primitive to re-locate the
    // face group). pushpull.axis is local, so transform it out to world.
    const worldAxis = prim.localToWorldNormal(pushpull.axis);
    const lastOp = {
        primitiveId: prim.id,
        normal:      worldAxis,
        centroid:    prim.faceGroupCentroid(pushpull.groupIdx),
        distance,
    };
    lastOp.centroid[0] += worldAxis[0] * distance;
    lastOp.centroid[1] += worldAxis[1] * distance;
    lastOp.centroid[2] += worldAxis[2] * distance;

    const newPositions = new Float32Array(pushpull.workingPositions);
    const newIndices   = new Uint32Array(pushpull.workingIndices);
    const newNormals   = new Float32Array(pushpull.workingNormals);
    // The surgery output already has correct face-group assignments
    // (carried via triToGroup). Pass them in via priorTriToGroup so
    // computeFaceGroups preserves identity instead of re-grouping by
    // coplanarity (which would re-merge bridge groups with caps if their
    // normals happen to align after a degenerate edge).
    prim.updateGeometry(newPositions, newIndices, newNormals,
                        { priorTriToGroup: pushpull.workingTriToGroup,
                          cleanRender: true });

    if (meshChanged(prevMesh, prim)) {
        const nextMesh = captureMesh(prim);
        history.record('Push/pull',
            () => applyMesh(prim, nextMesh),
            () => applyMesh(prim, prevMesh));
    }

    pickInfo.textContent = `extruded ${distance.toFixed(3)}`;
    clearPushPull();
    clearHighlight();
    MeasureBox.clear(measureBoxState);
    MeasureBox.setLastOp(measureBoxState, lastOp);
    MeasureBox.setActive(measureBoxState, true);
    renderMeasureBox();
    updateGizmoForActive();
}

function cancelPushPull() {
    if (!pushpull.active) return;
    pushpull.primitive.revertMesh();
    pickInfo.textContent = 'push/pull cancelled';
    clearPushPull();
    clearHighlight();
    MeasureBox.clearLastOp(measureBoxState);
    MeasureBox.clear(measureBoxState);
    MeasureBox.setActive(measureBoxState, false);
    renderMeasureBox();
}

function clearPushPull() {
    pushpull.active = false;
    pushpull.primitive = null;
    pushpull.faceTriangles = null;
    pushpull.snapPositions = null;
    pushpull.snapIndices = null;
    pushpull.snapNormals = null;
    pushpull.snapTriToGroup = null;
    pushpull.snapFaceGroups = null;
    pushpull.workingPositions = null;
    pushpull.workingIndices = null;
    pushpull.workingNormals = null;
    pushpull.workingTriToGroup = null;
    pushpull.workingFaceTris = null;
    pushpull.prevMesh = null;
    activeSnap = null;
}

// --- Move tool --------------------------------------------------------------
//
// Translate a whole primitive. Drag plane is camera-facing through the grab
// pivot. Inference snaps (excluding the moving primitive) override the plane
// intersection when present — delta = snapPos - pivot, mirroring SketchUp's
// "drag from pivot to snap target" behavior.

const moveToolState = MoveTool.createState();

function beginMove(object, hit) {
    if (!object || !hit) return;
    // Pure-transform move — snapshot TRS only, not mesh buffers. Move tool
    // operates on any SceneObject (primitive, group, component instance).
    moveToolState.prevTransform = captureTransform(object);
    MoveTool.begin(moveToolState, object, hit.position, cameraForward());
    pickInfo.textContent = `move  [${object.name}]  0`;
}

function commitMove() {
    if (!moveToolState.active) return;
    const obj = moveToolState.object;
    const prevT = moveToolState.prevTransform;
    const result = MoveTool.commit(moveToolState);
    if (result) {
        const d = result.delta;
        pickInfo.textContent = `moved [${result.object.name}] by ` +
            `[${d[0].toFixed(3)}, ${d[1].toFixed(3)}, ${d[2].toFixed(3)}]`;
        if (transformChanged(prevT, obj)) {
            const nextT = captureTransform(obj);
            history.record('Move',
                () => applyTransform(obj, nextT),
                () => applyTransform(obj, prevT));
        }
    }
    moveToolState.prevTransform = null;
    showSnapMarker(null);
    clearHighlight();
    updateGizmoForActive();
}

function cancelMove() {
    if (!moveToolState.active) return;
    MoveTool.cancel(moveToolState);
    moveToolState.prevTransform = null;
    pickInfo.textContent = 'move cancelled';
    showSnapMarker(null);
    clearHighlight();
}

// --- Gizmo (translate / rotate / scale) -------------------------------------
//
// The engine (bro.gizmo) renders the handles (arrows / rings / scale boxes),
// hit-tests the cursor, runs the drag, and fires per-frame delta callbacks.
// We own: anchoring the pivot to the active primitive's centroid, dispatching
// those deltas to MoveTool / RotateTool / ScaleTool for mesh preview + commit,
// and tracking drag state for UI feedback.
//
// The gizmo mode follows the current tool — setTool('move'|'rotate'|'scale')
// flips the engine-side mode. For the 'select' and 'pushpull' tools the gizmo
// stays hidden (picking competes with handles, and push/pull has its own
// per-face pivot).

const rotateToolState = RotateTool.createState();
const scaleToolState  = ScaleTool.createState();

// --- Rectangle tool --------------------------------------------------------
//
// Click-click drawing: first click anchors corner 0 on the sketch plane,
// second click commits corner 2 and creates a filled face primitive. The
// tool module is pure state; preview rendering, input routing, and
// primitive creation live here in the app.

const rectangleToolState = RectangleTool.createState();
let rectPreviewNode = null;

// Default sketch plane: the ground (XZ at y=0). Used as the fallback when
// the cursor isn't hovering any face. u/v aligned to world X/-Z so typed
// W,H map cleanly to the world axes.
function currentSketchPlane() {
    const normal = [0, 1, 0];
    const { u, v } = Sketch.worldAxisBasis(normal);
    return { origin: [0, 0, 0], normal, u, v };
}

// Resolve the sketch plane to use given a world-space ray. A raycast hit
// yields the hovered face's plane (anchored at the hit point); a miss
// yields the default ground plane. Mirrors SketchUp's inference-driven
// sketch plane — rectangles, circles, etc. draw on whichever face the
// user clicked. Axis-aligned faces get world-aligned u/v; arbitrary
// orientations fall back to planeBasis.
function resolveSketchPlaneFromRay(ray) {
    const pick = registry.pickAt(ray.origin, ray.dir);
    if (pick && pick.hit) {
        const normal = pick.hit.normal.slice();
        const { u, v } = Sketch.worldAxisBasis(normal);
        return { origin: pick.hit.position.slice(), normal, u, v,
                 onPrimitiveId: pick.primitive.id };
    }
    return currentSketchPlane();
}

function resolveSketchPlane(cx, cy) {
    return resolveSketchPlaneFromRay(screenToRay(cx, cy));
}

function destroyRectPreview() {
    if (rectPreviewNode) { rectPreviewNode.destroy(); rectPreviewNode = null; }
}

function refreshRectPreview() {
    const mesh = RectangleTool.buildMesh(rectangleToolState);
    if (!mesh) { destroyRectPreview(); return; }
    if (!rectPreviewNode) {
        rectPreviewNode = scene.createMesh({
            data:  mesh,
            color: '#ffa502',
            unlit: true,
            name:  'rect-preview',
        });
    } else {
        rectPreviewNode.updateMesh({
            positions: mesh.positions,
            indices:   mesh.indices,
            normals:   mesh.normals,
        });
    }
}

function beginRectangle(pos, plane) {
    RectangleTool.begin(rectangleToolState, plane || currentSketchPlane(), pos);
    pickInfo.textContent = 'rectangle  [corner 1 set — click for corner 2]';
    // Arm the VCB in pair mode so the user can type "W,H + Enter" to
    // finish the rectangle at exact dimensions, SketchUp-style.
    MeasureBox.clearLastOp(measureBoxState);
    MeasureBox.clear(measureBoxState);
    MeasureBox.setPairMode(measureBoxState, true);
    MeasureBox.setActive(measureBoxState, true);
    renderMeasureBox();
}

// Snap the rectangle's live corner to exact (w, h) in the plane's (u, v)
// basis. The sign of each dimension follows the cursor's current quadrant
// so typing "2,3" respects the direction the user dragged toward.
function applyRectangleDimensions(w, h) {
    const st = rectangleToolState;
    if (!st.active) return false;
    const c0 = st.corner0;
    const c1 = st.corner1;
    const { u, v } = st.plane;
    const du = (c1[0] - c0[0]) * u[0] + (c1[1] - c0[1]) * u[1] + (c1[2] - c0[2]) * u[2];
    const dv = (c1[0] - c0[0]) * v[0] + (c1[1] - c0[1]) * v[1] + (c1[2] - c0[2]) * v[2];
    const signU = du < 0 ? -1 : 1;
    const signV = dv < 0 ? -1 : 1;
    const newU = signU * Math.abs(w);
    const newV = signV * Math.abs(h);
    const newC1 = [
        c0[0] + newU * u[0] + newV * v[0],
        c0[1] + newU * u[1] + newV * v[1],
        c0[2] + newU * u[2] + newV * v[2],
    ];
    RectangleTool.update(st, newC1);
    refreshRectPreview();
    return true;
}

function updateRectangleAt(pos) {
    if (!rectangleToolState.active) return;
    RectangleTool.update(rectangleToolState, pos);
    refreshRectPreview();
    const sz = RectangleTool.size(rectangleToolState);
    if (sz) {
        pickInfo.textContent =
            `rectangle  ${sz.w.toFixed(3)} × ${sz.h.toFixed(3)}`;
    }
}

function commitRectangle() {
    if (!rectangleToolState.active) return;
    const mesh = RectangleTool.commit(rectangleToolState);
    destroyRectPreview();
    MeasureBox.setPairMode(measureBoxState, false);
    MeasureBox.clear(measureBoxState);
    MeasureBox.setActive(measureBoxState, false);
    renderMeasureBox();
    if (!mesh) {
        pickInfo.textContent = 'rectangle cancelled (zero area)';
        return;
    }
    // Capture buffers as typed arrays so redo can rebuild the primitive
    // without depending on the JS Mesh wrapper's lifetime.
    const data = {
        positions: new Float32Array(mesh.positions),
        indices:   new Uint32Array(mesh.indices),
        normals:   new Float32Array(mesh.normals),
    };
    const idx = registry.primitives.length;
    const spec = {
        name:  'Rectangle ' + (idx + 1),
        color: OUTLINER_COLORS[idx % OUTLINER_COLORS.length],
    };
    const id = registry.nextId();
    history.do('Add ' + spec.name,
        () => { registry.createFromMesh(spec, data, id); },
        () => { registry.remove(id); });
    pickInfo.textContent = `added ${spec.name}`;
    // After an add, stay in rectangle mode so the user can draw another —
    // matches SketchUp's "tool sticks after commit" UX.
}

function cancelRectangle() {
    if (!rectangleToolState.active) return;
    RectangleTool.cancel(rectangleToolState);
    destroyRectPreview();
    MeasureBox.setPairMode(measureBoxState, false);
    MeasureBox.clear(measureBoxState);
    MeasureBox.setActive(measureBoxState, false);
    renderMeasureBox();
    pickInfo.textContent = 'rectangle cancelled';
}

// --- Circle tool -----------------------------------------------------------
//
// Click-center + click-radius. Commit produces a triangulated 32-gon
// primitive ready for push/pull. VCB single-value input: type "R + Enter"
// for an exact radius.

const circleToolState = CircleTool.createState();
let circlePreviewNode = null;

function destroyCirclePreview() {
    if (circlePreviewNode) { circlePreviewNode.destroy(); circlePreviewNode = null; }
}

function refreshCirclePreview() {
    const mesh = CircleTool.buildMesh(circleToolState);
    if (!mesh) { destroyCirclePreview(); return; }
    if (!circlePreviewNode) {
        circlePreviewNode = scene.createMesh({
            data:  mesh,
            color: '#ffa502',
            unlit: true,
            name:  'circle-preview',
        });
    } else {
        circlePreviewNode.updateMesh({
            positions: mesh.positions,
            indices:   mesh.indices,
            normals:   mesh.normals,
        });
    }
}

function beginCircle(pos, plane) {
    CircleTool.begin(circleToolState, plane || currentSketchPlane(), pos);
    pickInfo.textContent = 'circle  [center set — move + click for radius]';
    MeasureBox.clearLastOp(measureBoxState);
    MeasureBox.clear(measureBoxState);
    MeasureBox.setPairMode(measureBoxState, false);
    MeasureBox.setActive(measureBoxState, true);
    renderMeasureBox();
}

function updateCircleAt(pos) {
    if (!circleToolState.active) return;
    CircleTool.update(circleToolState, pos);
    refreshCirclePreview();
    pickInfo.textContent =
        `circle  r = ${CircleTool.radius(circleToolState).toFixed(3)}`;
}

// Snap the live edge to an exact radius along the cursor's current
// direction from center — typed-radius respects the cursor bearing.
function applyCircleRadius(r) {
    const st = circleToolState;
    if (!st.active) return false;
    const c0 = st.center;
    const c1 = st.edge;
    const dx = c1[0] - c0[0], dy = c1[1] - c0[1], dz = c1[2] - c0[2];
    const L = Math.hypot(dx, dy, dz);
    let ux, uy, uz;
    if (L > 1e-9) {
        ux = dx / L; uy = dy / L; uz = dz / L;
    } else {
        // Cursor still on the center — pick the plane's u axis as a default
        // direction so the preview appears on Enter.
        ux = st.plane.u[0]; uy = st.plane.u[1]; uz = st.plane.u[2];
    }
    const rr = Math.abs(r);
    const newEdge = [c0[0] + ux * rr, c0[1] + uy * rr, c0[2] + uz * rr];
    CircleTool.update(st, newEdge);
    refreshCirclePreview();
    return true;
}

function commitCircle() {
    if (!circleToolState.active) return;
    const mesh = CircleTool.commit(circleToolState);
    destroyCirclePreview();
    MeasureBox.clear(measureBoxState);
    MeasureBox.setActive(measureBoxState, false);
    renderMeasureBox();
    if (!mesh) {
        pickInfo.textContent = 'circle cancelled (zero radius)';
        return;
    }
    const data = {
        positions: new Float32Array(mesh.positions),
        indices:   new Uint32Array(mesh.indices),
        normals:   new Float32Array(mesh.normals),
    };
    const idx = registry.primitives.length;
    const spec = {
        name:  'Circle ' + (idx + 1),
        color: OUTLINER_COLORS[idx % OUTLINER_COLORS.length],
    };
    const id = registry.nextId();
    history.do('Add ' + spec.name,
        () => { registry.createFromMesh(spec, data, id); },
        () => { registry.remove(id); });
    pickInfo.textContent = `added ${spec.name}`;
}

function cancelCircle() {
    if (!circleToolState.active) return;
    CircleTool.cancel(circleToolState);
    destroyCirclePreview();
    MeasureBox.clear(measureBoxState);
    MeasureBox.setActive(measureBoxState, false);
    renderMeasureBox();
    pickInfo.textContent = 'circle cancelled';
}

// --- Line tool -------------------------------------------------------------
//
// Click-chain polyline. First click locks the sketch plane (face-pick or
// ground fallback); subsequent clicks extend the polyline on that plane.
// A click that lands on a prior polyline vertex closes the sub-loop and
// commits a filled face primitive. Orphan prefix edges are discarded
// (pure-edge primitives are a future addition — MVP keeps to filled faces).
//
// Esc / tool-switch / double-click end the chain without creating anything.

const lineToolState = LineTool.createState();
let linePreviewNode = null;
const LINE_PREVIEW_COLOR = [1.0, 0.647, 0.008, 1.0];    // SketchUp orange
const LINE_PREVIEW_THICKNESS = 0.015;
// World-space tolerance for "this click closed the loop". Matches the
// inference snap radius in spirit — the cursor resolves to a plane hit and
// we ask if that hit is within CLOSE_EPS of any polyline vertex.
const LINE_CLOSE_EPS = 0.05;

function destroyLinePreview() {
    if (linePreviewNode) { linePreviewNode.destroy(); linePreviewNode = null; }
}

function refreshLinePreview() {
    const st = lineToolState;
    if (!st.active || st.points.length === 0) {
        destroyLinePreview();
        return;
    }
    // Assemble positions = committed polyline + preview cursor.
    const hasPreview = st.preview != null;
    const vCount = st.points.length + (hasPreview ? 1 : 0);
    const positions = new Float32Array(vCount * 3);
    for (let i = 0; i < st.points.length; i++) {
        positions[i * 3 + 0] = st.points[i][0];
        positions[i * 3 + 1] = st.points[i][1];
        positions[i * 3 + 2] = st.points[i][2];
    }
    if (hasPreview) {
        const last = vCount - 1;
        positions[last * 3 + 0] = st.preview[0];
        positions[last * 3 + 1] = st.preview[1];
        positions[last * 3 + 2] = st.preview[2];
    }
    // Edges: committed segments (N-1) + one rubber-band from last committed
    // point to the preview cursor (only when preview differs from the last
    // committed point).
    const edges = [];
    for (let i = 0; i < st.points.length - 1; i++) {
        edges.push({ a: i, b: i + 1 });
    }
    if (hasPreview) {
        edges.push({ a: st.points.length - 1, b: st.points.length });
    }
    if (edges.length === 0) { destroyLinePreview(); return; }
    const data = EdgeMesh.buildEdgeMesh(positions, edges, {
        thickness: LINE_PREVIEW_THICKNESS,
        color:     LINE_PREVIEW_COLOR,
    });
    if (!linePreviewNode) {
        linePreviewNode = scene.createMesh({
            positions: data.positions,
            normals:   data.normals,
            colors:    data.colors,
            indices:   data.indices,
            unlit:     true,
            name:      'line-preview',
        });
    } else {
        linePreviewNode.updateMesh({
            positions: data.positions,
            normals:   data.normals,
            colors:    data.colors,
            indices:   data.indices,
        });
    }
}

// Map a canvas-space click to a plane-locked 3D position, preferring a
// closure-snap to a polyline vertex when the cursor is near one. Returns
// { position, closureIndex } or null when the ray doesn't hit the plane.
function resolveLinePoint(cx, cy) {
    const st = lineToolState;
    if (!st.active || !st.plane) return null;
    const ray = screenToRay(cx, cy);
    const hit = Sketch.rayToPlane(ray, st.plane.origin, st.plane.normal);
    if (!hit) return null;
    const closureIndex = LineTool.findClosureIndex(st, hit, LINE_CLOSE_EPS);
    if (closureIndex >= 0) {
        return { position: st.points[closureIndex].slice(), closureIndex };
    }
    return { position: hit, closureIndex: -1 };
}

function beginLine(pos, plane) {
    LineTool.begin(lineToolState, plane || currentSketchPlane(), pos);
    refreshLinePreview();
    pickInfo.textContent = 'line  [click to extend · close to finish]';
}

function updateLineAt(pos) {
    if (!lineToolState.active) return;
    LineTool.update(lineToolState, pos);
    refreshLinePreview();
    const closureIdx = LineTool.findClosureIndex(
        lineToolState, pos, LINE_CLOSE_EPS);
    if (closureIdx >= 0) {
        pickInfo.textContent = 'line  [click to close loop ✕]';
    }
}

function addLinePoint(pos) {
    if (!lineToolState.active) return null;
    // Capture the plane BEFORE the call — LineTool.addPoint clears state on
    // closure, which would otherwise leave us holding a nulled plane.
    const plane = lineToolState.plane;
    const result = LineTool.addPoint(lineToolState, pos, LINE_CLOSE_EPS);
    if (result.kind === 'closed') {
        destroyLinePreview();
        finalizeLineFace(result.polygon, plane);
        // Orphan prefix (the dangling tail before the closure point) becomes
        // a free-standing edge primitive instead of being discarded.
        if (result.orphan && result.orphan.length >= 2) {
            persistOrphanEdges(result.orphan, 'Polyline');
        }
        return result;
    }
    refreshLinePreview();
    return result;
}

// The raw polygon coming out of LineTool is in user-click order. For
// Mesh.polygon3D to emit front-facing tris toward +plane.normal, the
// polygon must be CCW in the (u, v) basis. Flip the order if the signed
// area is negative.
// Filter out triangles whose 3D area is below SLIVER_AREA_MIN. Ear-clipping
// on near-collinear input sometimes emits sub-pixel slivers that break
// coplanarity heuristics downstream. Returns a fresh indices array (never
// larger than input).
const SLIVER_AREA_MIN = 1e-10;
function dropSliverTris(positions, indices) {
    const out = [];
    for (let t = 0; t < indices.length; t += 3) {
        const i0 = indices[t]*3, i1 = indices[t+1]*3, i2 = indices[t+2]*3;
        const ax = positions[i1]   - positions[i0];
        const ay = positions[i1+1] - positions[i0+1];
        const az = positions[i1+2] - positions[i0+2];
        const bx = positions[i2]   - positions[i0];
        const by = positions[i2+1] - positions[i0+1];
        const bz = positions[i2+2] - positions[i0+2];
        const cx = ay*bz - az*by;
        const cy = az*bx - ax*bz;
        const cz = ax*by - ay*bx;
        const area = 0.5 * Math.sqrt(cx*cx + cy*cy + cz*cz);
        if (area >= SLIVER_AREA_MIN) {
            out.push(indices[t], indices[t+1], indices[t+2]);
        }
    }
    return out;
}

function finalizeLineFace(polygon, plane) {
    if (!polygon || polygon.length < 3) {
        pickInfo.textContent = 'line  (sub-loop too small, discarded)';
        resetLineChain();
        return;
    }
    // Project to 2D, then split self-intersections (if any) into one or
    // more simple CCW sub-loops. A figure-8 produces two loops; a simple
    // polygon produces one. This keeps Line-tool output robust to crossings
    // without surfacing an error to the user.
    const uv = new Array(polygon.length);
    for (let i = 0; i < polygon.length; i++) {
        uv[i] = Sketch.project3Dto2D(polygon[i], plane.origin, plane.u, plane.v);
    }
    const subLoops = Sketch.splitSelfIntersectingPolygon(uv);
    if (subLoops.length === 0) {
        pickInfo.textContent = 'line  (degenerate polygon, discarded)';
        resetLineChain();
        return;
    }
    const createdNames = [];
    for (const loop2D of subLoops) {
        // Lift 2D sub-loop back into world space along the plane basis.
        const loop3D = loop2D.map(p =>
            [plane.origin[0] + p[0]*plane.u[0] + p[1]*plane.v[0],
             plane.origin[1] + p[0]*plane.u[1] + p[1]*plane.v[1],
             plane.origin[2] + p[0]*plane.u[2] + p[1]*plane.v[2]]);
        const flat = Sketch.flatten3D(loop3D);
        const mesh = Mesh.polygon3D(flat, [], plane.normal);
        if (!mesh || mesh.vertexCount === 0) continue;
        // Drop near-zero-area triangles (sliver triangulations from
        // near-collinear inputs) — they confuse face-group detection and
        // produce weird shading artifacts on extrusion.
        const cleanIdx = dropSliverTris(mesh.positions, mesh.indices);
        if (cleanIdx.length === 0) continue;
        const data = {
            positions: new Float32Array(mesh.positions),
            indices:   new Uint32Array(cleanIdx),
            normals:   new Float32Array(mesh.normals),
        };
        const slotIdx = registry.primitives.length + createdNames.length;
        const spec = {
            name:  'Polygon ' + (slotIdx + 1),
            color: OUTLINER_COLORS[slotIdx % OUTLINER_COLORS.length],
        };
        const id = registry.nextId();
        history.do('Add ' + spec.name,
            () => { registry.createFromMesh(spec, data, id); },
            () => { registry.remove(id); });
        createdNames.push(spec.name);
    }
    if (createdNames.length === 0) {
        pickInfo.textContent = 'line  (face triangulation failed)';
    } else if (createdNames.length === 1) {
        pickInfo.textContent = `added ${createdNames[0]}`;
    } else {
        pickInfo.textContent = `added ${createdNames.length} faces (` +
            `split at ${subLoops.length - 1} crossing` +
            (subLoops.length - 1 === 1 ? '' : 's') + ')';
    }
    resetLineChain();
}

// End the chain without closing. Any pending polyline (>=2 points) becomes
// a free-standing EdgePrimitive so the user keeps what they drew.
function commitLine() {
    if (!lineToolState.active) return;
    const orphan = lineToolState.points.slice();
    LineTool.commit(lineToolState);
    destroyLinePreview();
    persistOrphanEdges(orphan, 'Polyline');
}

// Build an EdgePrimitive from a 3D polyline (open). Wraps creation in a
// history command so undo removes it. No-op for <2 points.
function persistOrphanEdges(polyline, baseName) {
    if (!polyline || polyline.length < 2) {
        pickInfo.textContent = 'line chain ended';
        return;
    }
    const positions = new Float32Array(polyline.length * 3);
    for (let i = 0; i < polyline.length; i++) {
        positions[i*3]   = polyline[i][0];
        positions[i*3+1] = polyline[i][1];
        positions[i*3+2] = polyline[i][2];
    }
    const edges = [];
    for (let i = 0; i < polyline.length - 1; i++) {
        edges.push({ a: i, b: i + 1 });
    }
    const slotIdx = registry.primitives.length +
        registry.root.children.filter(c => c.kind === 'edge-primitive').length;
    const spec = {
        name:  (baseName || 'Edges') + ' ' + (slotIdx + 1),
        color: OUTLINER_COLORS[slotIdx % OUTLINER_COLORS.length],
    };
    const id = registry.nextId();
    history.do('Add ' + spec.name,
        () => { registry.createEdgePrimitive(spec, { positions, edges }, id); },
        () => { registry.remove(id); });
    pickInfo.textContent = 'added ' + spec.name +
        ' (' + edges.length + ' edge' + (edges.length === 1 ? '' : 's') + ')';
}

function cancelLine() {
    if (!lineToolState.active) return;
    LineTool.cancel(lineToolState);
    destroyLinePreview();
    pickInfo.textContent = 'line cancelled';
}

function resetLineChain() {
    LineTool.cancel(lineToolState);
    destroyLinePreview();
}

// --- Arc tool --------------------------------------------------------------
//
// SketchUp-classic 2-point + bulge arc. Three clicks: start, end, bulge.
// Output is an open polyline persisted as an EdgePrimitive (orphan edges).
// Inference snaps work on every click so an arc can chain off existing
// vertices/midpoints/edges. The sketch plane is captured on the first click
// (face-pick or ground fallback) and stays locked for clicks 2 and 3.

const arcToolState = ArcTool.createState();
let arcPreviewNode = null;

function destroyArcPreview() {
    if (arcPreviewNode) { arcPreviewNode.destroy(); arcPreviewNode = null; }
}

// Build a thin-prism edge mesh from the current preview polyline. We re-use
// EdgeMesh so the preview matches the persisted EdgePrimitive's appearance.
function refreshArcPreview() {
    const poly = ArcTool.buildPolyline(arcToolState);
    if (!poly || poly.length < 2) { destroyArcPreview(); return; }
    const positions = new Float32Array(poly.length * 3);
    for (let i = 0; i < poly.length; i++) {
        positions[i*3]   = poly[i][0];
        positions[i*3+1] = poly[i][1];
        positions[i*3+2] = poly[i][2];
    }
    const edges = [];
    for (let i = 0; i < poly.length - 1; i++) edges.push({ a: i, b: i + 1 });
    const data = EdgeMesh.buildEdgeMesh(positions, edges, {
        thickness: 0.014,
        color:     [1.0, 0.65, 0.01, 1.0],
    });
    if (!arcPreviewNode) {
        arcPreviewNode = scene.createMesh({
            positions: data.positions,
            normals:   data.normals,
            colors:    data.colors,
            indices:   data.indices,
            unlit:     true,
            name:      'arc-preview',
        });
    } else {
        arcPreviewNode.updateMesh({
            positions: data.positions,
            normals:   data.normals,
            colors:    data.colors,
            indices:   data.indices,
        });
    }
}

function beginArc(pos, plane) {
    ArcTool.begin(arcToolState, plane || currentSketchPlane(), pos);
    refreshArcPreview();
    pickInfo.textContent = 'arc  [click endpoint]';
}

function updateArcAt(pos) {
    if (!ArcTool.active(arcToolState)) return;
    ArcTool.update(arcToolState, pos);
    refreshArcPreview();
}

// Click 2: lock the chord endpoint. Returns true if accepted.
function setArcEnd(pos) {
    const ok = ArcTool.setEnd(arcToolState, pos, 1e-4);
    if (ok) {
        refreshArcPreview();
        pickInfo.textContent = 'arc  [click bulge point]';
    }
    return ok;
}

function commitArc() {
    if (!ArcTool.active(arcToolState)) return;
    const poly = ArcTool.commit(arcToolState);
    destroyArcPreview();
    if (!poly || poly.length < 2) {
        pickInfo.textContent = 'arc cancelled (degenerate)';
        return;
    }
    persistOrphanEdges(poly, 'Arc');
}

function cancelArc() {
    if (!ArcTool.active(arcToolState)) return;
    ArcTool.cancel(arcToolState);
    destroyArcPreview();
    pickInfo.textContent = 'arc cancelled';
}

// --- Offset tool -----------------------------------------------------------
//
// Pick a face → drag → produce a new face primitive coplanar with the source,
// inset (negative distance) or expanded (positive). The source face is left
// intact for the MVP — proper "inset ring + retriangulate the source"
// behaviour is a TODO that needs PolyMesh face splitting.

const offsetToolState = OffsetTool.createState();
let offsetPreviewNode = null;

function destroyOffsetPreview() {
    if (offsetPreviewNode) { offsetPreviewNode.destroy(); offsetPreviewNode = null; }
}

// Extract the world-space boundary loop of a face group via EditMesh. Returns
// an array of [x,y,z] in CCW order around the face (inherited from the
// half-edge winding), or null when the face has multiple boundary loops
// (holes — not supported by the offset MVP).
function faceGroupBoundaryWorld(prim, gIdx) {
    // The primitive's cached editMesh has no group tags (face.group === -1
    // throughout) — push/pull rebuilds its own group-tagged EditMesh per
    // operation. Mirror that pattern for offset.
    const em = EditMesh.fromMeshData(
        prim.positions, prim.indices, prim.faceGroups.triToGroup);
    const loops = EditMesh.findFaceGroupBoundary(em, gIdx);
    if (!loops || loops.length !== 1) return null;
    const loop = loops[0];
    if (loop.length < 3) return null;
    const w = prim.getWorldMatrix();
    return loop.map(he => {
        const local = [he.origin.x, he.origin.y, he.origin.z];
        return Mat4Lib.transformPoint(w, local);
    });
}

// Build the in-plane 2D basis for a face group, anchored at the loop's
// first vertex so loop coordinates start at (0, 0).
function planeForFaceGroup(prim, gIdx, loopWorld) {
    const groups = prim.faceGroups.groups;
    const localN = groups[gIdx].normal;
    const worldN = prim.localToWorldNormal(localN);
    const wn = Sketch.v3norm(worldN);
    const basis = Sketch.worldAxisBasis(wn);
    return {
        origin: loopWorld[0].slice(),
        normal: wn,
        u:      basis.u,
        v:      basis.v,
    };
}

function refreshOffsetPreview() {
    const loop = OffsetTool.buildOffsetLoop(offsetToolState);
    if (!loop || loop.length < 3) { destroyOffsetPreview(); return; }
    // Build a thin closed polyline preview using EdgeMesh, so the preview
    // doesn't z-fight with the source face.
    const positions = new Float32Array(loop.length * 3);
    for (let i = 0; i < loop.length; i++) {
        positions[i*3]   = loop[i][0];
        positions[i*3+1] = loop[i][1];
        positions[i*3+2] = loop[i][2];
    }
    const edges = [];
    for (let i = 0; i < loop.length; i++) {
        edges.push({ a: i, b: (i + 1) % loop.length });
    }
    const data = EdgeMesh.buildEdgeMesh(positions, edges, {
        thickness: 0.014,
        color:     [1.0, 0.65, 0.01, 1.0],
    });
    if (!offsetPreviewNode) {
        offsetPreviewNode = scene.createMesh({
            positions: data.positions,
            normals:   data.normals,
            colors:    data.colors,
            indices:   data.indices,
            unlit:     true,
            name:      'offset-preview',
        });
    } else {
        offsetPreviewNode.updateMesh({
            positions: data.positions,
            normals:   data.normals,
            colors:    data.colors,
            indices:   data.indices,
        });
    }
}

// Begin offset on a primitive's face group. `clickPos` is the world-space
// pick point on the face. Returns true if started, false if the face has
// holes or is otherwise unsupported.
function beginOffset(prim, gIdx, clickPos) {
    const loop = faceGroupBoundaryWorld(prim, gIdx);
    if (!loop) {
        pickInfo.textContent = 'offset: face boundary unsupported (multi-loop?)';
        return false;
    }
    const plane = planeForFaceGroup(prim, gIdx, loop);
    OffsetTool.begin(offsetToolState, prim, gIdx, loop, plane, clickPos);
    refreshOffsetPreview();
    pickInfo.textContent = 'offset  [drag for distance · click to commit · Esc cancel]';
    MeasureBox.clearLastOp(measureBoxState);
    MeasureBox.clear(measureBoxState);
    MeasureBox.setPairMode(measureBoxState, false);
    MeasureBox.setActive(measureBoxState, true);
    renderMeasureBox();
    return true;
}

function updateOffsetAt(cursorWorld) {
    if (!OffsetTool.active(offsetToolState)) return;
    OffsetTool.updateFromCursor(offsetToolState, cursorWorld);
    refreshOffsetPreview();
    pickInfo.textContent =
        `offset  d = ${offsetToolState.distance.toFixed(3)}`;
}

function applyOffsetDistance(d) {
    if (!OffsetTool.active(offsetToolState)) return false;
    OffsetTool.setDistance(offsetToolState, d);
    refreshOffsetPreview();
    return true;
}

function commitOffset() {
    if (!OffsetTool.active(offsetToolState)) return;
    const sourcePrim = offsetToolState.primitive;
    const planeNormal = offsetToolState.plane.normal.slice();
    const loop3D = OffsetTool.commit(offsetToolState);
    destroyOffsetPreview();
    MeasureBox.clear(measureBoxState);
    MeasureBox.setActive(measureBoxState, false);
    renderMeasureBox();
    if (!loop3D || loop3D.length < 3) {
        pickInfo.textContent = 'offset cancelled (collapsed or zero distance)';
        return;
    }
    const flat = Sketch.flatten3D(loop3D);
    const mesh = Mesh.polygon3D(flat, [], planeNormal);
    if (!mesh || mesh.vertexCount === 0) {
        pickInfo.textContent = 'offset failed (triangulation empty)';
        return;
    }
    const data = {
        positions: new Float32Array(mesh.positions),
        indices:   new Uint32Array(mesh.indices),
        normals:   new Float32Array(mesh.normals),
    };
    const slotIdx = registry.primitives.length;
    const spec = {
        name:  'Offset ' + (slotIdx + 1),
        color: OUTLINER_COLORS[slotIdx % OUTLINER_COLORS.length],
    };
    const id = registry.nextId();
    history.do('Add ' + spec.name,
        () => { registry.createFromMesh(spec, data, id); },
        () => { registry.remove(id); });
    pickInfo.textContent = `added ${spec.name}`;
}

function cancelOffset() {
    if (!OffsetTool.active(offsetToolState)) return;
    OffsetTool.cancel(offsetToolState);
    destroyOffsetPreview();
    MeasureBox.clear(measureBoxState);
    MeasureBox.setActive(measureBoxState, false);
    renderMeasureBox();
    pickInfo.textContent = 'offset cancelled';
}

// --- Follow-me tool --------------------------------------------------------
//
// Sweep a profile face along a path. UX:
//   1. The active selection (set via outliner) must be an EdgePrimitive
//      that holds the path polyline.
//   2. With the Follow-Me tool, click a face on any primitive to use as
//      the profile.
// The tool sweeps immediately and creates a new triangulated primitive.
// Source profile and path are left intact.

const followMeToolState = FollowMeTool.createState();

// Convert an EdgePrimitive into an ordered 3D world-space polyline. The
// edges array might be in any order; we walk from a degree-1 vertex (or
// any if the polyline is a closed loop) and chain via shared endpoints.
// Returns null when the edge set isn't a single simple polyline.
function edgePrimitiveToPath(edgePrim) {
    if (!edgePrim || edgePrim.kind !== 'edge-primitive') return null;
    const adj = new Map();    // vert idx → [other vert idx, ...]
    const N = edgePrim.positions.length / 3;
    for (const e of edgePrim.edges) {
        if (!adj.has(e.a)) adj.set(e.a, []);
        if (!adj.has(e.b)) adj.set(e.b, []);
        adj.get(e.a).push(e.b);
        adj.get(e.b).push(e.a);
    }
    // Pick a start: a degree-1 vertex if any (open polyline), else any.
    let start = -1;
    for (const [v, ns] of adj) {
        if (ns.length === 1) { start = v; break; }
    }
    if (start < 0) {
        const first = adj.keys().next();
        if (first.done) return null;
        start = first.value;
    }
    // Walk.
    const orderLocal = [];
    const visited = new Set();
    let cur = start;
    let prev = -1;
    while (cur != null && !visited.has(cur)) {
        visited.add(cur);
        orderLocal.push(cur);
        const ns = adj.get(cur);
        let next = null;
        for (const v of ns) {
            if (v !== prev && !visited.has(v)) { next = v; break; }
        }
        prev = cur;
        cur  = next;
    }
    if (orderLocal.length < 2) return null;
    // Local → world.
    const w = edgePrim.getWorldMatrix();
    return orderLocal.map(idx => Mat4Lib.transformPoint(w, [
        edgePrim.positions[idx*3],
        edgePrim.positions[idx*3 + 1],
        edgePrim.positions[idx*3 + 2],
    ]));
}

// Run a follow-me with the given (face primitive, group idx, edge primitive
// path). Wraps creation in a history command. Returns the new primitive
// id or null.
function runFollowMe(profilePrim, profileGroup, pathEdgePrim) {
    const loopWorld = faceGroupBoundaryWorld(profilePrim, profileGroup);
    if (!loopWorld) {
        pickInfo.textContent = 'follow-me: profile face boundary unsupported';
        return null;
    }
    const localN = profilePrim.faceGroups.groups[profileGroup].normal;
    const worldN = profilePrim.localToWorldNormal(localN);
    const path   = edgePrimitiveToPath(pathEdgePrim);
    if (!path || path.length < 2) {
        pickInfo.textContent = 'follow-me: path must be a simple polyline';
        return null;
    }
    FollowMeTool.beginWithProfile(
        followMeToolState, profilePrim, profileGroup, loopWorld, worldN);
    const mesh = FollowMeTool.commitWithPath(followMeToolState, path);
    if (!mesh) {
        pickInfo.textContent = 'follow-me: sweep failed';
        return null;
    }
    const data = {
        positions: mesh.positions,
        indices:   mesh.indices,
        normals:   mesh.normals,
    };
    const slotIdx = registry.primitives.length;
    const spec = {
        name:  'Sweep ' + (slotIdx + 1),
        color: OUTLINER_COLORS[slotIdx % OUTLINER_COLORS.length],
    };
    const id = registry.nextId();
    history.do('Add ' + spec.name,
        () => { registry.createFromMesh(spec, data, id); },
        () => { registry.remove(id); });
    pickInfo.textContent = `added ${spec.name}`;
    return id;
}

function cancelFollowMe() {
    if (!FollowMeTool.active(followMeToolState)) return;
    FollowMeTool.cancel(followMeToolState);
    pickInfo.textContent = 'follow-me cancelled';
}

// --- Tape measure ----------------------------------------------------------
//
// Two-click distance readout via inference snaps. No geometry is produced —
// the tool just updates the HUD's pick-info field. First click sets the
// "from" point; the live cursor shows running distance; second click
// displays the final distance and resets for another measurement.

const tapeToolState = TapeTool.createState();

function beginTape(pos) {
    TapeTool.begin(tapeToolState, pos);
    pickInfo.textContent = 'tape  [click second point]';
}

function updateTapeAt(pos) {
    if (!tapeToolState.active) return;
    TapeTool.update(tapeToolState, pos);
    const d = TapeTool.distance(tapeToolState);
    pickInfo.textContent = `tape  distance = ${d.toFixed(3)}`;
}

function commitTape() {
    if (!tapeToolState.active) return;
    const d = TapeTool.commit(tapeToolState);
    pickInfo.textContent = `measured ${d.toFixed(3)}`;
    return d;
}

function cancelTape() {
    if (!tapeToolState.active) return;
    TapeTool.cancel(tapeToolState);
    pickInfo.textContent = 'tape cancelled';
}

// --- Eraser tool -----------------------------------------------------------
//
// Click a FACE (not a primitive) to remove it. If that was the only face of
// the primitive, the whole primitive goes — so erasing a ground rectangle
// still works as "delete this shape." The outliner's trash-can button keeps
// doing whole-primitive delete. Edge-level erasing would require picking
// edges (not wired today — the BVH only sees face triangles) so this MVP
// stops at face granularity.

// Erase the face group at `groupIdx` on `primitive`. If it's the last
// face, fall through to whole-primitive delete. Undo restores the prior
// geometry; face groups recompute from scratch on both legs.
function eraseFace(primitive, groupIdx) {
    if (!primitive || !primitive.faceGroups) return;
    const group = primitive.faceGroups.groups[groupIdx];
    if (!group) return;
    if (primitive.faceGroups.groups.length === 1) {
        // Erasing the only face is effectively deleting the shape.
        deletePrimitive(primitive);
        return;
    }
    // Snapshot for undo — positions/normals are preserved even though some
    // vertex indices may become orphaned by the index filter. Orphaned
    // vertices are cheap memory-wise and harmless for BVH / inference (both
    // only see indices that still reference them).
    const prevPos = new Float32Array(primitive.positions);
    const prevIdx = new Uint32Array(primitive.indices);
    const prevNrm = primitive.normals ? new Float32Array(primitive.normals) : null;
    const droppedTris = new Set(group.tris);
    const newTriCount = (primitive.indices.length / 3) - droppedTris.size;
    const newIdx = new Uint32Array(newTriCount * 3);
    let dst = 0;
    const triCount = primitive.indices.length / 3;
    for (let t = 0; t < triCount; t++) {
        if (droppedTris.has(t)) continue;
        newIdx[dst * 3 + 0] = primitive.indices[t * 3 + 0];
        newIdx[dst * 3 + 1] = primitive.indices[t * 3 + 1];
        newIdx[dst * 3 + 2] = primitive.indices[t * 3 + 2];
        dst++;
    }
    const newPos = new Float32Array(primitive.positions);
    const newNrm = primitive.normals ? new Float32Array(primitive.normals) : null;
    // If the erase left a drag bound to this primitive, cancel it — stale
    // face-group indices would otherwise point into freed tris.
    if (pushpull.active && pushpull.primitive && pushpull.primitive.id === primitive.id) {
        cancelPushPull();
    }
    if (moveToolState.active && moveToolState.object && moveToolState.object.id === primitive.id) {
        cancelMove();
    }
    if (highlightPrimitive && highlightPrimitive.id === primitive.id) clearHighlight();
    history.do('Erase face on ' + primitive.name,
        () => primitive.updateGeometry(newPos, newIdx, newNrm),
        () => primitive.updateGeometry(prevPos, prevIdx, prevNrm));
}

function deletePrimitive(p) {
    if (!p) return;
    if (gizmoDrag.active && gizmoDrag.primitive && gizmoDrag.primitive.id === p.id) {
        cancelGizmoDrag();
    }
    if (pushpull.active && pushpull.primitive && pushpull.primitive.id === p.id) {
        cancelPushPull();
    }
    if (moveToolState.active && moveToolState.object && moveToolState.object.id === p.id) {
        cancelMove();
    }
    if (highlightPrimitive && highlightPrimitive.id === p.id) clearHighlight();
    if (p.kind === 'edge-primitive') {
        const snap = registry.snapshotEdgePrimitive(p);
        history.do('Delete ' + p.name,
            () => registry.remove(snap.id),
            () => registry.restoreEdgePrimitiveFromSnapshot(snap));
        return;
    }
    const snap = registry.snapshotPrimitive(p);
    history.do('Delete ' + p.name,
        () => registry.remove(snap.id),
        () => registry.restoreFromSnapshot(snap));
}

// Drag state — begin/translate/rotate/scale/end fires from the engine update
// this block so the rest of the app (UI, highlight refresh) can react.
const gizmoDrag = {
    active:      false,
    mode:        'translate',  // 'translate'|'rotate'|'scale'
    primitive:   null,    // back-compat alias for `object`; captured at begin
    object:      null,    // any SceneObject: primitive, group, or instance
    axis:        [0, 0, 0],
    pivot:       [0, 0, 0],
    totalDelta:  [0, 0, 0], // accumulated world-space translate delta
    prevTransform: null,    // TRS at drag start — used for undo
};

// World-space bbox centroid. Primitive now stores local-space positions —
// Primitive.worldCentroid() composes through the transform chain. For
// non-primitive SceneObjects (groups, component instances) we walk the
// subtree's primitive leaves and average their world centroids.
function primCentroid(obj) {
    if (!obj) return [0, 0, 0];
    if (obj.worldCentroid) return obj.worldCentroid();
    // Group / component-instance: average of descendant-primitive centroids.
    let sx = 0, sy = 0, sz = 0, n = 0;
    obj.traverseLeaves(function (leaf) {
        if (!leaf.worldCentroid) return;
        const c = leaf.worldCentroid();
        sx += c[0]; sy += c[1]; sz += c[2]; n++;
    });
    if (n === 0) {
        return [obj.translation[0], obj.translation[1], obj.translation[2]];
    }
    return [sx / n, sy / n, sz / n];
}

// Keep the gizmo anchored to the active primitive's bbox centroid. Hidden
// when no primitive is active OR the current tool doesn't use the gizmo.
// Called from applyCamera + after any edit that changes the active primitive.
function updateGizmoForActive() {
    const prim = registry.active;
    const mode = GIZMO_TOOLS[currentTool];
    if (!prim || !prim.visible || !mode) {
        bro.gizmo.hide();
        return;
    }
    bro.gizmo.setMode(mode);
    const c = primCentroid(prim);
    bro.gizmo.setPosition(c[0], c[1], c[2]);
    bro.gizmo.show();
}

function axisLabel(ax) {
    if (Math.abs(ax[0]) > 0.5) return 'X';
    if (Math.abs(ax[1]) > 0.5) return 'Y';
    return 'Z';
}

// Engine→app handler wiring. The `position` callback fires every frame so
// the pivot automatically tracks the active primitive — we don't have to
// call setPosition from the rest of the app any more.
bro.gizmo.attach({
    position: () => {
        const prim = registry.active;
        if (!prim || !prim.visible) return [0, 0, 0];
        // With TRS-based tools, transforms mutate the SceneObject directly,
        // so the composed world centroid already reflects the drag state —
        // no manual totalDelta fixup needed (move/rotate/scale all route
        // through setTranslation/setRotation/setScale which invalidate the
        // world matrix).
        return primCentroid(prim);
    },
    beginDrag: () => {
        const obj = registry.active;
        if (!obj) return;
        const mode = GIZMO_TOOLS[currentTool] || 'translate';
        const axisName = bro.gizmo.hovered;
        const axis = axisName === 'x' ? [1, 0, 0]
                  : axisName === 'y' ? [0, 1, 0]
                  : axisName === 'z' ? [0, 0, 1]
                  :                    [0, 0, 0];
        const c = primCentroid(obj);
        gizmoDrag.active = true;
        gizmoDrag.mode = mode;
        gizmoDrag.primitive = obj;   // back-compat alias
        gizmoDrag.object    = obj;
        gizmoDrag.axis[0] = axis[0];
        gizmoDrag.axis[1] = axis[1];
        gizmoDrag.axis[2] = axis[2];
        gizmoDrag.pivot[0] = c[0];
        gizmoDrag.pivot[1] = c[1];
        gizmoDrag.pivot[2] = c[2];
        gizmoDrag.totalDelta[0] = 0;
        gizmoDrag.totalDelta[1] = 0;
        gizmoDrag.totalDelta[2] = 0;
        gizmoDrag.prevTransform = captureTransform(obj);
        if      (mode === 'translate') MoveTool.begin(moveToolState, obj, c, axis);
        else if (mode === 'rotate')    RotateTool.begin(rotateToolState, obj, c);
        else if (mode === 'scale')     ScaleTool.begin(scaleToolState,  obj, c);
        pickInfo.textContent =
            `gizmo  ${mode}  ${axisName ? axisName.toUpperCase() : ''}  [${obj.name}]`;
    },
    translate: (dx, dy, dz) => {
        if (!gizmoDrag.active || gizmoDrag.mode !== 'translate') return;
        gizmoDrag.totalDelta[0] += dx;
        gizmoDrag.totalDelta[1] += dy;
        gizmoDrag.totalDelta[2] += dz;
        MoveTool.applyDelta(moveToolState,
            gizmoDrag.totalDelta[0],
            gizmoDrag.totalDelta[1],
            gizmoDrag.totalDelta[2]);
        if (highlightPrimitive && highlightPrimitive.id === gizmoDrag.primitive.id) {
            refreshHighlight();
        }
        const d = gizmoDrag.totalDelta[0] * gizmoDrag.axis[0]
                + gizmoDrag.totalDelta[1] * gizmoDrag.axis[1]
                + gizmoDrag.totalDelta[2] * gizmoDrag.axis[2];
        pickInfo.textContent =
            `gizmo  ${axisLabel(gizmoDrag.axis)}  [${gizmoDrag.primitive.name}]  ` +
            `${d.toFixed(3)}`;
    },
    rotate: (qx, qy, qz, qw) => {
        if (!gizmoDrag.active || gizmoDrag.mode !== 'rotate') return;
        RotateTool.applyDelta(rotateToolState, qx, qy, qz, qw);
        if (highlightPrimitive && highlightPrimitive.id === gizmoDrag.primitive.id) {
            refreshHighlight();
        }
        const q = rotateToolState.accumQ;
        const ang = 2 * Math.acos(Math.min(1, Math.abs(q[3])));
        pickInfo.textContent =
            `gizmo  rotate  [${gizmoDrag.primitive.name}]  ` +
            `${(ang * 180 / Math.PI).toFixed(1)}\u00B0`;
    },
    scale: (sx, sy, sz) => {
        if (!gizmoDrag.active || gizmoDrag.mode !== 'scale') return;
        ScaleTool.applyDelta(scaleToolState, sx, sy, sz);
        if (highlightPrimitive && highlightPrimitive.id === gizmoDrag.primitive.id) {
            refreshHighlight();
        }
        const a = scaleToolState.accumScale;
        pickInfo.textContent =
            `gizmo  scale  [${gizmoDrag.primitive.name}]  ` +
            `[${a[0].toFixed(3)}, ${a[1].toFixed(3)}, ${a[2].toFixed(3)}]`;
    },
    endDrag: () => {
        if (!gizmoDrag.active) return;
        const obj = gizmoDrag.object;
        const mode = gizmoDrag.mode;
        const prevT = gizmoDrag.prevTransform;
        let result = null;
        if      (mode === 'translate') result = MoveTool.commit(moveToolState);
        else if (mode === 'rotate')    result = RotateTool.commit(rotateToolState);
        else if (mode === 'scale')     result = ScaleTool.commit(scaleToolState);
        if (result) {
            if (mode === 'translate') {
                const d = result.delta;
                pickInfo.textContent = `moved [${obj.name}] by ` +
                    `[${d[0].toFixed(3)}, ${d[1].toFixed(3)}, ${d[2].toFixed(3)}]`;
            } else if (mode === 'rotate') {
                const q = result.quat;
                const ang = 2 * Math.acos(Math.min(1, Math.abs(q[3])));
                pickInfo.textContent = `rotated [${obj.name}] by ` +
                    `${(ang * 180 / Math.PI).toFixed(1)}\u00B0`;
            } else if (mode === 'scale') {
                const s = result.scale;
                pickInfo.textContent = `scaled [${obj.name}] by ` +
                    `[${s[0].toFixed(3)}, ${s[1].toFixed(3)}, ${s[2].toFixed(3)}]`;
            }
            if (transformChanged(prevT, obj)) {
                const nextT = captureTransform(obj);
                const label = mode === 'translate' ? 'Move'
                            : mode === 'rotate'    ? 'Rotate'
                                                   : 'Scale';
                history.record(label,
                    () => applyTransform(obj, nextT),
                    () => applyTransform(obj, prevT));
            }
        }
        if (obj && obj.kind === 'primitive' && highlightPrimitive &&
            highlightPrimitive.id === obj.id) {
            refreshHighlight();
        }
        gizmoDrag.active = false;
        gizmoDrag.primitive = null;
        gizmoDrag.object    = null;
        gizmoDrag.prevTransform = null;
        showSnapMarker(null);
        updateGizmoForActive();
    },
});

function cancelGizmoDrag() {
    if (!gizmoDrag.active) return;
    const obj = gizmoDrag.object;
    const mode = gizmoDrag.mode;
    if      (mode === 'translate') MoveTool.cancel(moveToolState);
    else if (mode === 'rotate')    RotateTool.cancel(rotateToolState);
    else if (mode === 'scale')     ScaleTool.cancel(scaleToolState);
    if (obj && obj.kind === 'primitive' && highlightPrimitive &&
        highlightPrimitive.id === obj.id) {
        refreshHighlight();
    }
    gizmoDrag.active = false;
    gizmoDrag.primitive = null;
    gizmoDrag.object    = null;
    gizmoDrag.prevTransform = null;
    pickInfo.textContent = 'gizmo cancelled';
    showSnapMarker(null);
    updateGizmoForActive();
}

// --- Inference snap marker --------------------------------------------------

const SNAP_SHAPES = {
    'endpoint': '<circle cx="9" cy="9" r="5" fill="#2ecc71" stroke="#fff" stroke-width="1.5"/>',
    'midpoint': '<polygon points="9,2 16,9 9,16 2,9" fill="none" stroke="#1abc9c" stroke-width="2"/>',
    'on-edge':  '<rect x="3" y="3" width="12" height="12" fill="none" stroke="#e74c3c" stroke-width="2"/>',
    'on-face':  '<polygon points="9,3 15,9 9,15 3,9" fill="#3498db" fill-opacity="0.4" stroke="#3498db" stroke-width="1.5"/>',
};

const SNAP_SPHERE_RADIUS = 0.04;
const snapSphereMesh = Mesh.sphere(SNAP_SPHERE_RADIUS, 12, 8);
const snapSpheres = {};
for (const [type, color] of Object.entries(Inference._COLOR)) {
    const node = scene.createMesh({
        data: snapSphereMesh,
        color,
        unlit: true,
        name: 'snap-sphere-' + type,
    });
    node.visible = false;
    snapSpheres[type] = node;
}

let activeSnap = null;

// --- Measurement Box (VCB) --------------------------------------------------

const measureBoxState = MeasureBox.createState();
const measureBoxEl    = document.getElementById('measure-box');

function renderMeasureBox() {
    if (!measureBoxEl) return;
    if (!measureBoxState.active) {
        measureBoxEl.style.display = 'none';
        return;
    }
    measureBoxEl.style.display = 'block';
    const buf = measureBoxState.buffer;
    const display = buf.length ? buf : '';
    const pair = measureBoxState.pairMode;
    const label = pair ? 'Dimensions' : 'Distance';
    let hint;
    if (pair) {
        hint = 'Type <b>W,H</b> + Enter for exact size · Esc cancel';
    } else if (measureBoxState.lastOp && !pushpull.active) {
        hint = 'Type distance + Enter to re-extrude · Esc to dismiss';
    } else {
        hint = 'Type exact distance + Enter · Esc cancel';
    }
    measureBoxEl.innerHTML =
        '<div class="label">' + label + '</div>' +
        '<div><span class="value">' + (display || '&mdash;') + '</span>' +
        '<span class="caret"></span></div>' +
        '<div class="hint">' + hint + '</div>';
}
renderMeasureBox();

// Re-apply the last committed push/pull at a new distance on the same
// primitive it originally ran on. Normal-match + centroid-proximity relocate
// the pushed face on the post-commit geometry.
function redoLastPushPull(distance) {
    const op = measureBoxState.lastOp;
    if (!op) return false;
    const prim = registry.getById(op.primitiveId);
    if (!prim) return false;
    const gIdx = prim.findFaceGroupByNormal(op.normal, op.centroid);
    if (gIdx < 0) return false;
    const g = prim.faceGroups.groups[gIdx];
    const centroid = prim.faceGroupCentroid(gIdx);
    beginPushPull(prim, {
        triangleIndex: g.tris[0],
        position: centroid,
        normal: g.normal.slice(),
        distance: 0,
    });
    applyPushPull(distance);
    commitPushPull();
    return true;
}

function hideSnapSpheres() {
    for (const t in snapSpheres) snapSpheres[t].visible = false;
}

function showSnapMarker(snap) {
    activeSnap = snap;
    if (!snap) {
        snapMarker.style.display = 'none';
        if (snapInfo) snapInfo.textContent = '';
        hideSnapSpheres();
        return;
    }
    snapMarker.style.display = 'block';
    snapMarker.style.left = snap.screen.x + 'px';
    snapMarker.style.top  = snap.screen.y + 'px';
    snapMarker.innerHTML  = '<svg width="18" height="18">' +
        (SNAP_SHAPES[snap.type] || '') + '</svg>';
    if (snapInfo) {
        snapInfo.innerHTML = `snap: <b style="color:${snap.color}">${snap.label}</b>`;
    }
    hideSnapSpheres();
    const sph = snapSpheres[snap.type];
    if (sph) {
        sph.x = snap.position[0];
        sph.y = snap.position[1];
        sph.z = snap.position[2];
        sph.visible = true;
    }
}

const PUSHPULL_EXCLUDE = ['on-edge'];

// Resolve the best snap across every visible primitive. The on-face fallback
// uses registry.pickAt so it hits the nearest primitive under the ray.
// `excludePrimitiveId` filters both the inference geos and the on-face
// raycast — Move uses this so the moving primitive can't snap to itself.
function resolveSnap(cx, cy, ray, includeFaceFallback, excludeTypes, excludePrimitiveId) {
    const camOpts = Camera.orbitViewOpts(cam, canvas);
    const w = canvas.clientWidth || canvas.width;
    const h = canvas.clientHeight || canvas.height;
    const filterOpt = excludePrimitiveId != null
        ? { excludeId: excludePrimitiveId } : null;
    let onFaceHit = null;
    if (includeFaceFallback) {
        const pick = registry.pickAt(ray.origin, ray.dir, filterOpt);
        if (pick) onFaceHit = pick.hit;
    }
    return Inference.findSnap({
        cursorX: cx, cursorY: cy, ray,
        camOpts, width: w, height: h,
        geos: registry.collectInferenceGeos(filterOpt),
        onFaceHit,
        excludeTypes,
    });
}

function snapAxisDistance(snap, pivot, axis) {
    const dx = snap.position[0] - pivot[0];
    const dy = snap.position[1] - pivot[1];
    const dz = snap.position[2] - pivot[2];
    return dx * axis[0] + dy * axis[1] + dz * axis[2];
}

// --- Input: right=rotate, middle=pan, wheel=zoom, left=tool ----------------

let rightDown  = false;
let middleDown = false;

function updatePointerLock() {
    const want = rightDown || middleDown;
    const locked = document.pointerLockElement === canvas;
    if (want && !locked) canvas.requestPointerLock();
    else if (!want && locked) document.exitPointerLock();
}

function handleLeftDown(e) {
    const r = canvas.getBoundingClientRect();
    const cx = e.clientX - r.left;
    const cy = e.clientY - r.top;
    // The engine consumes the mousedown before it reaches us when the
    // cursor is on a gizmo arrow, so we never get here with a handle hit.
    // Keep the pivot current so the drag anchors correctly the moment the
    // user clicks.
    updateGizmoForActive();
    const pick = pickAt(cx, cy);
    pickInfo.textContent = formatHit(pick);
    // Legacy test hook: keep `__lastPick` in the hit shape (not {primitive,hit}).
    window.__lastPick = pick && pick.hit;
    if (currentTool === 'select') {
        if (pick) {
            // pick.object is the outermost SceneObject within the current
            // edit context (a group / component-instance wrapping the clicked
            // primitive, or the primitive itself if it's at context level).
            // Highlight still shows the INNERmost face group — feedback stays
            // pointed at the actual clicked face.
            registry.setActive(pick.object.id);
            setHighlightFaceGroup(
                pick.primitive,
                pick.primitive.faceGroups.triToGroup[pick.hit.triangleIndex]);
        } else {
            clearHighlight();
        }
    } else if (currentTool === 'pushpull') {
        if (pick) {
            // Push/pull always edits primitive-level geometry, so we operate
            // on the innermost leaf even when it's wrapped in a group —
            // SketchUp enters the group implicitly for this kind of edit.
            registry.setActive(pick.object.id);
            beginPushPull(pick.primitive, pick.hit);
        } else {
            clearHighlight();
        }
    } else if (currentTool === 'move') {
        if (pick) {
            registry.setActive(pick.object.id);
            beginMove(pick.object, pick.hit);
        } else {
            clearHighlight();
        }
    } else if (currentTool === 'rectangle') {
        // First click: resolve the sketch plane under the cursor (hovered
        // face, or ground fallback). Subsequent clicks stay locked to the
        // plane captured at begin — mirrors SketchUp's "you sketched on
        // this face, stay on this face" behavior.
        const plane = rectangleToolState.active
            ? rectangleToolState.plane : resolveSketchPlane(cx, cy);
        const hit = Sketch.rayToPlane(
            screenToRay(cx, cy), plane.origin, plane.normal);
        if (!hit) return;
        if (!rectangleToolState.active) {
            beginRectangle(hit, plane);
        } else {
            updateRectangleAt(hit);
            commitRectangle();
        }
    } else if (currentTool === 'circle') {
        const plane = circleToolState.active
            ? circleToolState.plane : resolveSketchPlane(cx, cy);
        const hit = Sketch.rayToPlane(
            screenToRay(cx, cy), plane.origin, plane.normal);
        if (!hit) return;
        if (!circleToolState.active) {
            beginCircle(hit, plane);
        } else {
            updateCircleAt(hit);
            commitCircle();
        }
    } else if (currentTool === 'arc') {
        // Stage 1 (idle): plane resolved from face-pick / ground; first click
        // begins. Stage 2 (await-end): same locked plane, second click sets
        // the chord. Stage 3 (await-bulge): third click commits.
        if (!ArcTool.active(arcToolState)) {
            const plane = resolveSketchPlane(cx, cy);
            const hit = Sketch.rayToPlane(
                screenToRay(cx, cy), plane.origin, plane.normal);
            if (!hit) return;
            beginArc(hit, plane);
            return;
        }
        const plane = arcToolState.plane;
        const hit = Sketch.rayToPlane(
            screenToRay(cx, cy), plane.origin, plane.normal);
        if (!hit) return;
        if (arcToolState.stage === 'await-end') {
            setArcEnd(hit);
        } else if (arcToolState.stage === 'await-bulge') {
            ArcTool.update(arcToolState, hit);
            commitArc();
        }
    } else if (currentTool === 'line') {
        if (!lineToolState.active) {
            const plane = resolveSketchPlane(cx, cy);
            const hit = Sketch.rayToPlane(
                screenToRay(cx, cy), plane.origin, plane.normal);
            if (!hit) return;
            beginLine(hit, plane);
        } else {
            const resolved = resolveLinePoint(cx, cy);
            if (!resolved) return;
            addLinePoint(resolved.position);
        }
    } else if (currentTool === 'followme') {
        // Profile = clicked face. Path = currently-active EdgePrimitive
        // (selected via the outliner before invoking the tool).
        if (!pick) {
            pickInfo.textContent = 'follow-me: click a face to use as profile';
            return;
        }
        const path = registry.active;
        if (!path || path.kind !== 'edge-primitive') {
            pickInfo.textContent = 'follow-me: select an edge primitive in ' +
                'the outliner first to use as the path';
            return;
        }
        const gIdx = pick.primitive.faceGroups.triToGroup[pick.hit.triangleIndex];
        runFollowMe(pick.primitive, gIdx, path);
    } else if (currentTool === 'offset') {
        // Stage 1: face under cursor → start offset on that face group.
        // Stage 2: cursor in plane → click commits.
        if (!OffsetTool.active(offsetToolState)) {
            if (!pick) return;
            const gIdx = pick.primitive.faceGroups.triToGroup[pick.hit.triangleIndex];
            beginOffset(pick.primitive, gIdx, pick.hit.position);
        } else {
            const plane = offsetToolState.plane;
            const hit = Sketch.rayToPlane(
                screenToRay(cx, cy), plane.origin, plane.normal);
            if (hit) updateOffsetAt(hit);
            commitOffset();
        }
    } else if (currentTool === 'erase') {
        if (pick) {
            const gIdx = pick.primitive.faceGroups.triToGroup[pick.hit.triangleIndex];
            eraseFace(pick.primitive, gIdx);
            pickInfo.textContent = 'erased face';
        } else {
            clearHighlight();
        }
    } else if (currentTool === 'tape') {
        const ray = screenToRay(cx, cy);
        const snap = resolveSnap(cx, cy, ray, true);
        if (!snap) return;    // nothing under the cursor — no-op
        if (!tapeToolState.active) {
            beginTape(snap.position);
        } else {
            updateTapeAt(snap.position);
            commitTape();
        }
    }
}

canvas.addEventListener('mousedown', (e) => {
    if (e.button === 0) {
        if (!pushpull.active && measureBoxState.lastOp) {
            MeasureBox.clearLastOp(measureBoxState);
            MeasureBox.clear(measureBoxState);
            MeasureBox.setActive(measureBoxState, false);
            renderMeasureBox();
        }
        handleLeftDown(e);
    } else if (e.button === 2) {
        // Right-click while drawing a line: end the chain without closing.
        // Mirrors SketchUp's right-click-to-finish affordance for polyline
        // tools. Falls through to orbit-drag if the line tool isn't active.
        if (lineToolState.active) { commitLine(); e.preventDefault(); return; }
        rightDown = true;
        e.preventDefault();
        updatePointerLock();
    } else if (e.button === 1) {
        middleDown = true;
        e.preventDefault();
        updatePointerLock();
    }
});
document.addEventListener('mouseup', (e) => {
    // Gizmo drag commit is handled by the engine's endDrag callback (see
    // bro.gizmo.attach) — nothing to do here for the gizmo path.
    if (e.button === 0 && pushpull.active) commitPushPull();
    else if (e.button === 0 && moveToolState.active) commitMove();
    if (e.button === 2) rightDown  = false;
    if (e.button === 1) middleDown = false;
    updatePointerLock();
});
document.addEventListener('mousemove', (e) => {
    if (rightDown)  { Camera.orbitLook(cam, e.movementX, e.movementY); applyCamera(); return; }
    if (middleDown) { Camera.orbitPan (cam, e.movementX, e.movementY); applyCamera(); return; }
    const r = canvas.getBoundingClientRect();
    const cx = e.clientX - r.left;
    const cy = e.clientY - r.top;
    const ray = screenToRay(cx, cy);
    // Gizmo drag is owned by the engine and consumes mousemove before it
    // reaches us — the drag logic lives in the `translate` callback in
    // bro.gizmo.attach above. We never see gizmoDrag.active here.
    if (rectangleToolState.active) {
        const plane = rectangleToolState.plane;
        const hit = Sketch.rayToPlane(ray, plane.origin, plane.normal);
        if (hit) updateRectangleAt(hit);
        return;
    }
    if (circleToolState.active) {
        const plane = circleToolState.plane;
        const hit = Sketch.rayToPlane(ray, plane.origin, plane.normal);
        if (hit) updateCircleAt(hit);
        return;
    }
    if (lineToolState.active) {
        const resolved = resolveLinePoint(cx, cy);
        if (resolved) updateLineAt(resolved.position);
        return;
    }
    if (ArcTool.active(arcToolState)) {
        const plane = arcToolState.plane;
        const hit = Sketch.rayToPlane(ray, plane.origin, plane.normal);
        if (hit) updateArcAt(hit);
        return;
    }
    if (OffsetTool.active(offsetToolState)) {
        const plane = offsetToolState.plane;
        const hit = Sketch.rayToPlane(ray, plane.origin, plane.normal);
        if (hit) updateOffsetAt(hit);
        return;
    }
    if (currentTool === 'offset' || currentTool === 'followme') {
        // Hover preview: highlight the candidate face group.
        const pick = pickAt(cx, cy);
        if (pick) {
            const gIdx = pick.primitive.faceGroups.triToGroup[pick.hit.triangleIndex];
            setHighlightFaceGroup(pick.primitive, gIdx);
        } else {
            clearHighlight();
        }
        return;
    }
    if (tapeToolState.active) {
        const snap = resolveSnap(cx, cy, ray, true);
        if (snap) {
            updateTapeAt(snap.position);
            showSnapMarker(snap);
        } else {
            showSnapMarker(null);
        }
        return;
    }
    if (currentTool === 'erase') {
        // Hover preview: highlight the face group the click would erase.
        const pick = pickAt(cx, cy);
        if (pick) {
            const gIdx = pick.primitive.faceGroups.triToGroup[pick.hit.triangleIndex];
            setHighlightFaceGroup(pick.primitive, gIdx);
        } else {
            clearHighlight();
        }
        return;
    }
    if (pushpull.active) {
        // Pushpull state (axis, pivot) is in the primitive's LOCAL frame;
        // surgery operates on local positions. Transform the world ray and
        // any inference snap into the same local frame before running the
        // distance math, so the result `t` directly feeds surgery.
        const pushPrim = pushpull.primitive;
        const pushId = pushPrim.id;
        const vcbVal = MeasureBox.parseValue(measureBoxState.buffer);
        if (vcbVal !== null) {
            showSnapMarker(resolveSnap(cx, cy, ray, false, PUSHPULL_EXCLUDE, pushId));
            return;
        }
        const snap = resolveSnap(cx, cy, ray, false, PUSHPULL_EXCLUDE, pushId);
        const localRay = worldRayToLocal(ray, pushPrim);
        let dist;
        if (snap) {
            const localSnapPos = pushPrim.worldToLocalPoint(snap.position);
            dist = snapAxisDistance({ position: localSnapPos },
                                    pushpull.pivot, pushpull.axis);
            showSnapMarker(snap);
        } else {
            dist = rayVsAxisDistance(localRay, pushpull.pivot, pushpull.axis);
            showSnapMarker(null);
        }
        applyPushPull(dist);
        return;
    }
    if (moveToolState.active) {
        const movingId = moveToolState.object.id;
        const snap = resolveSnap(cx, cy, ray, true, null, movingId);
        let target;
        if (snap) {
            target = snap.position;
            showSnapMarker(snap);
        } else {
            target = MoveTool.rayVsPlane(ray,
                moveToolState.pivot, moveToolState.planeNormal);
            showSnapMarker(null);
            if (!target) return;
        }
        const dx = target[0] - moveToolState.pivot[0];
        const dy = target[1] - moveToolState.pivot[1];
        const dz = target[2] - moveToolState.pivot[2];
        MoveTool.applyDelta(moveToolState, dx, dy, dz);
        pickInfo.textContent = `move  [${moveToolState.object.name}]  ` +
            `[${dx.toFixed(3)}, ${dy.toFixed(3)}, ${dz.toFixed(3)}]`;
        return;
    }
    // Keep the pivot current so the gizmo tracks geometry edits while
    // hovering. Hover highlighting is owned by the engine — if an arrow is
    // under the cursor the engine sets bro.gizmo.hovered and we suppress
    // the snap marker to avoid a double-highlight.
    updateGizmoForActive();
    if (bro.gizmo.hovered) {
        showSnapMarker(null);
        return;
    }
    showSnapMarker(resolveSnap(cx, cy, ray, false));
});
canvas.addEventListener('dblclick', (e) => {
    // Double-click ends the line chain (SketchUp polyline-finish gesture).
    if (lineToolState.active) { commitLine(); e.preventDefault(); return; }
    // Otherwise: if we double-clicked a group (or component instance),
    // enter its edit context — SketchUp-style "drill into group".
    const r = canvas.getBoundingClientRect();
    const pick = pickAt(e.clientX - r.left, e.clientY - r.top);
    if (pick && pick.object &&
        (pick.object.kind === 'group' || pick.object.kind === 'component-instance') &&
        pick.object !== registry.editContext) {
        registry.enterContext(pick.object);
        pickInfo.textContent = 'entered ' + pick.object.name;
        e.preventDefault();
    }
});
canvas.addEventListener('contextmenu', (e) => e.preventDefault());
canvas.addEventListener('auxclick',    (e) => { if (e.button === 1) e.preventDefault(); });
canvas.addEventListener('wheel', (e) => {
    const factor = Math.exp(e.deltaY * 0.001);
    cam.dist = Math.max(0.1, cam.dist * factor);
    applyCamera();
    e.preventDefault();
});

function handleMeasureBoxKey(key) {
    if (!measureBoxState.active) return false;
    const action = MeasureBox.feedKey(measureBoxState, key);
    if (action === 'ignored') return false;
    if (action === 'append') {
        renderMeasureBox();
        if (pushpull.active) {
            const v = MeasureBox.parseValue(measureBoxState.buffer);
            if (v !== null) applyPushPull(v);
        } else if (rectangleToolState.active) {
            const pair = MeasureBox.parseValuePair(measureBoxState.buffer);
            if (pair) applyRectangleDimensions(pair[0], pair[1]);
        } else if (circleToolState.active) {
            const r = MeasureBox.parseValue(measureBoxState.buffer);
            if (r !== null) applyCircleRadius(r);
        } else if (OffsetTool.active(offsetToolState)) {
            const d = MeasureBox.parseValue(measureBoxState.buffer);
            if (d !== null) applyOffsetDistance(d);
        }
        return true;
    }
    if (action === 'commit') {
        if (pushpull.active) {
            const v = MeasureBox.parseValue(measureBoxState.buffer);
            applyPushPull(v);
            commitPushPull();
        } else if (rectangleToolState.active) {
            const pair = MeasureBox.parseValuePair(measureBoxState.buffer);
            if (pair) {
                applyRectangleDimensions(pair[0], pair[1]);
                commitRectangle();
            }
        } else if (circleToolState.active) {
            const r = MeasureBox.parseValue(measureBoxState.buffer);
            if (r !== null) {
                applyCircleRadius(r);
                commitCircle();
            }
        } else if (OffsetTool.active(offsetToolState)) {
            const d = MeasureBox.parseValue(measureBoxState.buffer);
            if (d !== null) {
                applyOffsetDistance(d);
                commitOffset();
            }
        } else if (measureBoxState.lastOp) {
            const v = MeasureBox.parseValue(measureBoxState.buffer);
            MeasureBox.clear(measureBoxState);
            renderMeasureBox();
            redoLastPushPull(v);
        }
        return true;
    }
    if (action === 'cancel') {
        renderMeasureBox();
        if (pushpull.active) {
            cancelPushPull();
        } else if (rectangleToolState.active) {
            cancelRectangle();
        } else if (circleToolState.active) {
            cancelCircle();
        } else if (OffsetTool.active(offsetToolState)) {
            cancelOffset();
        } else {
            MeasureBox.clearLastOp(measureBoxState);
            MeasureBox.setActive(measureBoxState, false);
            renderMeasureBox();
        }
        return true;
    }
    return false;
}

document.addEventListener('keydown', (e) => {
    if (e.target && (e.target.tagName === 'INPUT' || e.target.tagName === 'TEXTAREA')) return;
    // Skip the contenteditable rename field so typed edits don't trigger
    // app shortcuts mid-rename.
    if (e.target && e.target.getAttribute &&
        e.target.getAttribute('contenteditable') === 'true') return;
    if (handleMeasureBoxKey(e.key)) { e.preventDefault(); return; }
    // Undo / redo / save / open / new. All Ctrl+<key>; suppressed mid-
    // gesture because a half-finished operation's buffers would confuse
    // history entries and a mid-drag save would capture a preview state.
    if ((e.ctrlKey || e.metaKey) && !gizmoDrag.active && !pushpull.active &&
        !moveToolState.active && !rectangleToolState.active &&
        !circleToolState.active && !lineToolState.active &&
        !ArcTool.active(arcToolState) &&
        !OffsetTool.active(offsetToolState)) {
        const k = (e.key || '').toLowerCase();
        if (k === 'z' && !e.shiftKey) {
            if (history.canUndo()) history.undo();
            e.preventDefault();
            return;
        }
        if ((k === 'z' && e.shiftKey) || k === 'y') {
            if (history.canRedo()) history.redo();
            e.preventDefault();
            return;
        }
        if (k === 's') {
            // Ctrl+S: save (falls back to save-as if no path);
            // Ctrl+Shift+S: always save-as.
            if (e.shiftKey || !proj.path) proj.saveAs();
            else                          proj.save();
            e.preventDefault();
            return;
        }
        if (k === 'o') {
            proj.open();
            e.preventDefault();
            return;
        }
        if (k === 'n') {
            proj.new();
            e.preventDefault();
            return;
        }
        if (k === 'g' && !e.shiftKey) {
            groupSelection();
            e.preventDefault();
            return;
        }
        if (k === 'g' && e.shiftKey) {
            ungroupSelection();
            e.preventDefault();
            return;
        }
    }
// Tool selection is driven by the toolbar UI. Only Escape lives as a
    // global shortcut because it's a modal cancel (no tool change).
    if (e.key === 'Escape') {
        // A modal gesture takes priority — cancel it. Only if no gesture is
        // active AND we're inside a group/component edit context, Escape
        // exits one level of context (SketchUp "pop out" gesture).
        let cancelled = false;
        if (gizmoDrag.active)                           { cancelGizmoDrag(); cancelled = true; }
        if (pushpull.active)                            { cancelPushPull(); cancelled = true; }
        if (moveToolState.active)                       { cancelMove(); cancelled = true; }
        if (rotateToolState && rotateToolState.active)  { RotateTool.cancel(rotateToolState); cancelled = true; }
        if (scaleToolState  && scaleToolState.active)   { ScaleTool.cancel(scaleToolState); cancelled = true; }
        if (rectangleToolState.active)                  { cancelRectangle(); cancelled = true; }
        if (circleToolState.active)                     { cancelCircle(); cancelled = true; }
        if (lineToolState.active)                       { cancelLine(); cancelled = true; }
        if (ArcTool.active(arcToolState))               { cancelArc(); cancelled = true; }
        if (OffsetTool.active(offsetToolState))         { cancelOffset(); cancelled = true; }
        if (tapeToolState.active)                       { cancelTape(); cancelled = true; }
        if (!cancelled && registry.editContext !== registry.root) {
            registry.exitContext();
            pickInfo.textContent = 'exited context';
        }
    }
});

// Toolbar — tool selection is DOM-driven; the handler funnels everything
// through setTool() which keeps gizmo mode, active-button state, and the
// HUD label in sync. Indexed loop because bro's NodeList isn't iterable
// (same pattern as the outliner below).
const toolButtons = document.querySelectorAll('.tool-btn');
for (let i = 0; i < toolButtons.length; i++) {
    const btn = toolButtons[i];
    btn.addEventListener('click', () => {
        const t = btn.getAttribute('data-tool');
        if (t) { setTool(t); return; }
        const a = btn.getAttribute('data-action');
        if (a === 'group')          groupSelection();
        else if (a === 'ungroup')   ungroupSelection();
        else if (a === 'make-component') makeComponentFromSelection();
    });
}

// --- Group / Ungroup / Make Component ---------------------------------------
//
// Operate on the current active selection. MVP has single-select; multi-
// select is a Phase-2-later follow-up. Group wraps the active object in a new
// Group; ungroup unwraps an active Group; make-component turns the active
// object into a ComponentDefinition + a single ComponentInstance at the same
// spot.

function groupSelection() {
    if (!registry.active) {
        pickInfo.textContent = 'group: nothing selected';
        return;
    }
    const members = [registry.active];
    const prev = members.map(m => ({ obj: m, parent: m.parent,
                                     index: m.parent.children.indexOf(m),
                                     t: m.translation.slice(),
                                     r: m.rotation.slice(),
                                     s: m.scale.slice() }));
    let created = null;
    history.do('Group',
        () => { created = registry.createGroup(members, { name: 'Group' }); },
        () => {
            // Undo: explode the group, then restore each member's original
            // parent + index + TRS (createGroup used reparentPreservingWorld,
            // which rewrote local TRS for the new parent frame).
            if (created) registry.explodeGroup(created);
            for (const p of prev) {
                if (p.obj.parent !== p.parent) p.parent.addChild(p.obj);
                p.obj.setTRS(p.t, p.r, p.s);
            }
            created = null;
        });
    updateGizmoForActive();
}

function ungroupSelection() {
    const target = registry.active;
    if (!target || target.kind !== 'group') {
        pickInfo.textContent = 'ungroup: no group selected';
        return;
    }
    const parent = target.parent;
    const index  = parent.children.indexOf(target);
    const members = target.children.slice();
    // Capture each child's pre-ungroup local TRS so redo can recreate the
    // exact group membership + transform.
    const before = members.map(m => ({ obj: m, t: m.translation.slice(),
                                       r: m.rotation.slice(),
                                       s: m.scale.slice() }));
    const groupTRS = { t: target.translation.slice(),
                       r: target.rotation.slice(),
                       s: target.scale.slice() };
    const groupId = target.id;
    const groupName = target.name;
    history.do('Ungroup',
        () => {
            registry.explodeGroup(registry.getById(groupId));
        },
        () => {
            // Redo-undo: rebuild the group and re-parent members. Uses
            // createGroup which relocates via reparentPreservingWorld, then
            // restores each child's pre-ungroup local TRS explicitly.
            const g = new Group({ id: groupId, name: groupName,
                                  translation: groupTRS.t,
                                  rotation:    groupTRS.r,
                                  scale:       groupTRS.s });
            parent.children.splice(index, 0, g);
            g.parent = parent;
            for (const rec of before) {
                if (rec.obj.parent) rec.obj.parent.removeChild(rec.obj);
                g.addChild(rec.obj);
                rec.obj.setTRS(rec.t, rec.r, rec.s);
            }
            g._invalidateWorld();
            registry.active = g;
            registry._emit();
        });
    updateGizmoForActive();
}

function makeComponentFromSelection() {
    if (!registry.active) {
        pickInfo.textContent = 'make-component: nothing selected';
        return;
    }
    // For MVP, go through registry.makeComponent directly without history.
    // Component creation + un-creation round-trip is invasive (move subtree
    // into definition / back) — supporting undo here requires the same
    // scaffolding as Group and is left as a follow-up.
    const r = registry.makeComponent([registry.active], { name: 'Component' });
    if (r) pickInfo.textContent = 'made component';
    updateGizmoForActive();
}

// --- Outliner panel --------------------------------------------------------
//
// Left-bottom DOM panel listing every primitive with add-dropdown + per-row
// visibility / delete / rename. Re-renders whenever the registry changes.

const outlinerListEl = document.getElementById('outliner-list');
const outlinerAddBtns = document.querySelectorAll('.outliner-add-btn');

// Palette for auto-assigned colors on new primitives (wrap-around). The
// default box uses index 0 — subsequent adds walk the palette.
const OUTLINER_COLORS = ['#b8bcc2'];

// New primitives spawn along +X offset from origin so they don't stack on
// the default box. Monotonically increasing — deletions don't reclaim slots
// so the UX is "every add goes to fresh space".
let _outlinerNextAddX = 3;

function outlinerStartRename(prim, spanEl) {
    spanEl.setAttribute('contenteditable', 'true');
    spanEl.focus();
    if (document.createRange && window.getSelection) {
        const range = document.createRange();
        range.selectNodeContents(spanEl);
        const sel = window.getSelection();
        sel.removeAllRanges();
        sel.addRange(range);
    }
    let finished = false;
    const commit = (save) => {
        if (finished) return;
        finished = true;
        spanEl.removeAttribute('contenteditable');
        spanEl.removeEventListener('keydown', onKey);
        spanEl.removeEventListener('blur', onBlur);
        if (!save) { outlinerRender(); return; }
        const name = (spanEl.textContent || '').trim();
        if (!name.length || name === prim.name) { outlinerRender(); return; }
        const prev = prim.name;
        history.do('Rename',
            () => registry.setName(prim.id, name),
            () => registry.setName(prim.id, prev));
    };
    const onBlur = () => commit(true);
    const onKey = (e) => {
        if (e.key === 'Enter') { e.preventDefault(); commit(true); }
        else if (e.key === 'Escape') { e.preventDefault(); commit(false); }
    };
    spanEl.addEventListener('blur', onBlur);
    spanEl.addEventListener('keydown', onKey);
}

// Collapsed-state bookkeeping for the tree. Keyed by object id so state
// survives structural re-renders. A group defaults to expanded.
const _outlinerCollapsed = new Set();

function outlinerRender() {
    if (!outlinerListEl) return;
    outlinerListEl.innerHTML = '';
    _renderBreadcrumb();
    for (const child of registry.root.children) {
        _outlinerRenderNode(child, 0);
    }
}

function _renderBreadcrumb() {
    const el = document.getElementById('context-breadcrumb');
    if (!el) return;
    if (registry.editContext === registry.root) { el.textContent = ''; return; }
    const chain = [];
    let n = registry.editContext;
    while (n && n !== registry.root) { chain.unshift(n); n = n.parent; }
    el.textContent = 'editing: ' + chain.map(o => o.name).join(' › ') +
        '  [Esc to exit]';
}

function _outlinerRenderNode(obj, depth) {
    const row = document.createElement('div');
    let cls = 'outliner-row';
    if (obj === registry.active) cls += ' active';
    if (!obj.visible) cls += ' hidden';
    if (obj === registry.editContext) cls += ' context-edit';
    row.className = cls;
    row.dataset.id = String(obj.id);
    row.style.paddingLeft = (6 + depth * 14) + 'px';
    row.addEventListener('click', () => registry.setActive(obj.id));

    const hasChildren = obj.kind === 'group' && obj.children.length > 0;
    const twist = document.createElement('button');
    twist.className = 'outliner-twist';
    twist.textContent = hasChildren
        ? (_outlinerCollapsed.has(obj.id) ? '\u25B6' : '\u25BC')
        : '\u00A0';
    twist.disabled = !hasChildren;
    twist.addEventListener('click', (e) => {
        e.stopPropagation();
        if (!hasChildren) return;
        if (_outlinerCollapsed.has(obj.id)) _outlinerCollapsed.delete(obj.id);
        else _outlinerCollapsed.add(obj.id);
        outlinerRender();
    });
    row.appendChild(twist);

    const vis = document.createElement('button');
    vis.className = 'outliner-vis';
    vis.textContent = obj.visible ? '\u25CF' : '\u25CB';
    vis.title = obj.visible ? 'Hide' : 'Show';
    vis.addEventListener('click', (e) => {
        e.stopPropagation();
        registry.setVisible(obj.id, !obj.visible);
    });
    row.appendChild(vis);

    const kind = document.createElement('span');
    kind.className = 'outliner-kind';
    kind.textContent = obj.kind === 'group'              ? '[G]'
                     : obj.kind === 'component-instance' ? '[C]'
                     : obj.kind === 'edge-primitive'     ? '[E]'
                                                          : '   ';
    row.appendChild(kind);

    const name = document.createElement('span');
    name.className = 'outliner-name';
    name.textContent = obj.name;
    name.title = 'Double-click to rename (or enter group/component)';
    name.addEventListener('dblclick', (e) => {
        e.stopPropagation();
        // Groups + component instances: dblclick enters edit context.
        // Primitives: dblclick renames.
        if (obj.kind === 'group' || obj.kind === 'component-instance') {
            registry.enterContext(obj);
            pickInfo.textContent = 'entered ' + obj.name;
        } else {
            outlinerStartRename(obj, name);
        }
    });
    row.appendChild(name);

    const del = document.createElement('button');
    del.className = 'outliner-del';
    del.textContent = '\u00D7';
    del.title = 'Delete';
    del.addEventListener('click', (e) => {
        e.stopPropagation();
        if (obj.kind === 'primitive' || obj.kind === 'edge-primitive') deletePrimitive(obj);
        else {
            // Group / component instance: destroy without history (full-
            // subtree undo support is a Phase-6 follow-up).
            if (obj.destroy) obj.destroy();
            registry._emit();
        }
    });
    row.appendChild(del);

    outlinerListEl.appendChild(row);

    if (obj.kind === 'group' && !_outlinerCollapsed.has(obj.id)) {
        for (const c of obj.children) _outlinerRenderNode(c, depth + 1);
    }
}

function outlinerAddPrimitive(type) {
    const idx = registry.primitives.length;
    const spec = {
        type,
        name: type[0].toUpperCase() + type.slice(1) + ' ' + (idx + 1),
        color: OUTLINER_COLORS[idx % OUTLINER_COLORS.length],
        position: [_outlinerNextAddX, 0, 0],
    };
    if (type === 'box')      spec.params = { sx: 1, sy: 1, sz: 1 };
    if (type === 'sphere')   spec.params = { r: 1, seg: 24, rings: 16 };
    if (type === 'cylinder') spec.params = { r: 0.8, h: 2, seg: 24 };
    if (type === 'plane')    spec.params = { w: 2, d: 2, sx: 1, sz: 1 };
    _outlinerNextAddX += 2.5;
    // Reserve the id up front so redo-of-add restores the same id and any
    // external references (lastOp, outliner reveal) stay valid.
    const id = registry.nextId();
    let created = null;
    history.do('Add ' + spec.name,
        () => { created = registry.createWithId(spec, id); },
        () => { registry.remove(id); });
    return created;
}

// bro's DOM NodeList isn't iterable with for..of — index-based loop.
if (outlinerAddBtns) {
    for (let i = 0; i < outlinerAddBtns.length; i++) {
        const btn = outlinerAddBtns[i];
        btn.addEventListener('click', () => outlinerAddPrimitive(btn.dataset.type));
    }
}

registry.onChange = function () {
    outlinerRender();
    updateGizmoForActive();
};
outlinerRender();
updateGizmoForActive();

// --- History HUD line -------------------------------------------------------

const historyInfoEl = document.getElementById('history-info');
function renderHistoryInfo() {
    if (!historyInfoEl) return;
    if (!history.canUndo() && !history.canRedo()) {
        historyInfoEl.innerHTML = 'history: <span style="color:#777">empty</span>';
        return;
    }
    const parts = ['history:'];
    if (history.canUndo()) {
        const last = history.entries()[history.size() - 1];
        parts.push('\u21B6 ' + last.label);
    }
    if (history.canRedo()) parts.push('\u21B7');
    historyInfoEl.textContent = parts.join('  ');
}
history.on('change', renderHistoryInfo);
renderHistoryInfo();

// --- Project title + dirty indicator ---------------------------------------

const projectTitleEl = document.getElementById('project-title');
function renderProjectTitle() {
    if (!projectTitleEl) return;
    const name = proj.name;
    const dirty = proj.isDirty() ? ' *' : '';
    projectTitleEl.textContent = name + dirty;
    projectTitleEl.style.color = proj.isDirty() ? '#ffa502' : '#bbb';
}
proj.on('change', renderProjectTitle);
renderProjectTitle();

// --- Test hook --------------------------------------------------------------
//
// Getters resolve lazily against `registry.active` so tests always see the
// current rebuilt state after push/pull commits, not a stale pre-edit
// binding. Legacy positional APIs (beginPushPull(hit), setHighlight...)
// assume the active primitive to stay compatible with the pre-refactor tests.

window.__editor = {
    scene, cam, sceneAxesNode,
    registry,
    pickAt, screenToRay,

    // Active-primitive shortcuts (compat with the single-primitive spike).
    get boxNode()      { return registry.active && registry.active.meshNode; },
    get boxMesh()      { return registry.active && registry.active.mesh; },
    get boxBVH()       { return registry.active && registry.active.bvh; },
    get boxPositions() { return registry.active && registry.active.positions; },
    get boxIndices()   { return registry.active && registry.active.indices; },
    get boxNormals()   { return registry.active && registry.active.normals; },
    get faceGroups()   { return registry.active && registry.active.faceGroups; },
    get editMesh()     { return registry.active && registry.active.editMesh; },
    get inferenceGeo() { return registry.active && registry.active.inferenceGeo; },
    get edgesNode()    { return registry.active && registry.active.edgesNode; },
    get highlightNode(){ return highlightNode; },
    get currentTool()  { return currentTool; },
    get activeSnap()   { return activeSnap; },

    clearHighlight,
    setHighlightTriangle:  (triIdx, normal) =>
        setHighlightTriangle(registry.active, triIdx, normal),
    setHighlightTriangles: (triIdxs, normal, positionsSrc, indicesSrc) =>
        setHighlightTriangles(registry.active, triIdxs, normal, positionsSrc, indicesSrc),
    setHighlightFaceGroup: (groupIdx) =>
        setHighlightFaceGroup(registry.active, groupIdx),
    computeFaceGroups: Primitive.computeFaceGroups,
    setTool,

    // Tools — primitive-aware API plus a legacy single-arg shim that assumes
    // the active primitive.
    beginPushPull:       (hit) => beginPushPull(registry.active, hit),
    beginPushPullOn:     (primitive, hit) => beginPushPull(primitive, hit),
    applyPushPull, commitPushPull, cancelPushPull,
    collectAffectedVertexIndices: (groupIdx) =>
        registry.active.collectAffectedVertexIndices(groupIdx),
    rayVsAxisDistance,
    get pushpull() { return pushpull; },

    // Move tool. beginMove/commitMove/cancelMove wrap the MoveTool module
    // with camera-derived plane-normal + scene-editor side effects (snap
    // marker, highlight). applyMoveDelta is a thin pass-through for tests
    // that bypass cursor resolution.
    beginMove,
    applyMoveDelta: (dx, dy, dz) => MoveTool.applyDelta(moveToolState, dx, dy, dz),
    commitMove, cancelMove,
    get moveToolState() { return moveToolState; },
    cameraForward,

    // Translate gizmo — rendering and drag live in the engine (bro.gizmo.*).
    // Expose the drag-state mirror + helpers so tests / tooling can still
    // inspect what the app is doing during a drag.
    primCentroid,
    updateGizmoForActive,
    cancelGizmoDrag,
    get gizmoDrag() { return gizmoDrag; },

    // Inference hooks.
    resolveSnap, snapAxisDistance, showSnapMarker,

    // VCB.
    measureBoxState, renderMeasureBox, handleMeasureBoxKey,
    redoLastPushPull,
    findFaceGroupByNormal: (n, ref) => registry.active.findFaceGroupByNormal(n, ref),
    faceGroupCentroid:     (gIdx)   => registry.active.faceGroupCentroid(gIdx),

    // Outliner hooks (UI is in DOM; tests can trigger the internal helpers
    // without synthesizing events).
    outlinerRender,
    outlinerAddPrimitive,

    // Undo / redo.
    history,
    captureMesh, applyMesh,

    // Project: save/load/new. serializeScene/deserializeScene/setupDefaultScene
    // are exposed for headless tests that want to round-trip without dialogs.
    proj,
    serializeScene, deserializeScene, setupDefaultScene,

    // Rectangle drawing tool. Expose state + functions so headless tests
    // can drive a draw cycle without synthesizing mouse events.
    get rectangleToolState() { return rectangleToolState; },
    beginRectangle, updateRectangleAt, commitRectangle, cancelRectangle,
    applyRectangleDimensions,
    currentSketchPlane, resolveSketchPlane, resolveSketchPlaneFromRay,

    // Circle tool
    get circleToolState() { return circleToolState; },
    beginCircle, updateCircleAt, commitCircle, cancelCircle,
    applyCircleRadius,

    // Eraser
    deletePrimitive, eraseFace,

    // Tape measure
    get tapeToolState() { return tapeToolState; },
    beginTape, updateTapeAt, commitTape, cancelTape,

    // Line tool
    get lineToolState() { return lineToolState; },
    beginLine, updateLineAt, addLinePoint, commitLine, cancelLine,
    resolveLinePoint,

    // Free-floating edges (orphan polylines, arc-tool output).
    persistOrphanEdges,

    // Arc tool. 3-click 2-point + bulge; output is an EdgePrimitive.
    get arcToolState() { return arcToolState; },
    beginArc, updateArcAt, setArcEnd, commitArc, cancelArc,

    // Offset tool. Pick a face, drag, commit a coplanar offset polygon.
    get offsetToolState() { return offsetToolState; },
    beginOffset, updateOffsetAt, applyOffsetDistance, commitOffset, cancelOffset,
    faceGroupBoundaryWorld,

    // Follow-me. Active EdgePrimitive = path; click face to commit sweep.
    get followMeToolState() { return followMeToolState; },
    runFollowMe, edgePrimitiveToPath, cancelFollowMe,
};
