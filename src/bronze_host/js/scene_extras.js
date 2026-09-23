// scene_extras.js — the SceneNode members over the hand-registered natives in
// native_scene_extras.cpp: the per-type accessors (alphaCutoff, doubleSided,
// interior, priority, emissiveColor, radius, fillColor / strokeColor /
// strokeWidth, autoSync, pixelsPerUnit, billboard, nearClipDist, frameIndex),
// the instanced-mesh operations (updateInstance, setInstancedMesh,
// setAtlasGrid, setScatterSegments, setTubeSegments, updateInstances) and
// PhysicsNode.syncToPhysics, plus the setter-method spellings (setAlphaCutoff,
// setDoubleSided) apps used before the accessors existed.
//
// Entered right after js/scene.js (installSceneModule, host_js_modules.cpp),
// which is what put SceneNode on globalThis; it also teaches createMesh /
// createInstancedMesh / createShape / createSprite the option keys these
// accessors answer for (emissiveColor, alphaCutoff, doubleSided, billboard,
// nearClipDist, atlas, scatter, tube, ...), so an option and a later
// assignment take the same path.
(function () {
    'use strict';

    const SceneNode = globalThis.SceneNode;
    const SceneGraph = globalThis.SceneGraph;
    if (typeof SceneNode !== 'function' || typeof SceneGraph !== 'function') return;

    const accessor = (obj, name, get, set) =>
        Object.defineProperty(obj, name, { get, set, enumerable: true, configurable: true });
    const fn = (obj, name, value) =>
        Object.defineProperty(obj, name, { value, writable: true, enumerable: true, configurable: true });
    const EMPTY_F32 = new Float32Array(0);
    const toF32 = (v) => v instanceof Float32Array ? v : (ArrayBuffer.isView(v) || Array.isArray(v) ? Float32Array.from(v) : EMPTY_F32);
    const typeOf = (node) => __bro_native.scene.SceneNode_type_get(node);
    const isMesh = (node) => { const t = typeOf(node); return t === 'mesh' || t === 'skinnedMesh'; };
    const isInstanced = (node) => typeOf(node) === 'instancedMesh';
    const isMeshOrInstanced = (node) => isMesh(node) || isInstanced(node);
    const isShape = (node) => typeOf(node) === 'shape';
    const isProbe = (node) => typeOf(node) === 'reflectionProbe';
    const isPhysics = (node) => typeOf(node) === 'physics';
    const isSprite = (node) => typeOf(node) === 'sprite';

    // A typed accessor: `get` answers undefined and `set` is a no-op off the
    // node types it belongs to, which is what the old per-type bindings did.
    const typed = (name, pred, get, set) => accessor(SceneNode.prototype, name,
        function () { return pred(this) ? get(this) : undefined; },
        function (v) { if (pred(this)) set(this, v); });

    typed('alphaCutoff', isMeshOrInstanced,
        (n) => __bro_native.scene.SceneNode_alphaCutoff_get(n),
        (n, v) => __bro_native.scene.SceneNode_alphaCutoff_set(n, +v));
    typed('doubleSided', isInstanced,
        (n) => __bro_native.scene.SceneNode_doubleSided_get(n),
        (n, v) => __bro_native.scene.SceneNode_doubleSided_set(n, !!v));
    typed('nearClipDist', isMeshOrInstanced,
        (n) => __bro_native.scene.SceneNode_nearClipDist_get(n),
        (n, v) => __bro_native.scene.SceneNode_nearClipDist_set(n, +v));
    typed('emissiveColor', isMeshOrInstanced,
        (n) => Array.from(__bro_native.scene.SceneNode_emissiveColor_get(n)),
        (n, v) => {
            if (typeof v === 'string') __bro_native.scene.SceneNode_emissiveColorCss_set(n, v);
            else if (v && typeof v.length === 'number') {
                const c = new Float64Array(v.length);
                for (let i = 0; i < v.length; i++) c[i] = +v[i];
                __bro_native.scene.SceneNode_emissiveColor_set(n, c);
            }
        });
    typed('interior', isProbe,
        (n) => __bro_native.scene.SceneNode_interior_get(n),
        (n, v) => __bro_native.scene.SceneNode_interior_set(n, +v));
    typed('priority', isProbe,
        (n) => __bro_native.scene.SceneNode_priority_get(n),
        (n, v) => __bro_native.scene.SceneNode_priority_set(n, v | 0));
    typed('radius', isShape,
        (n) => __bro_native.scene.SceneNode_radius_get(n),
        (n, v) => __bro_native.scene.SceneNode_radius_set(n, +v));
    typed('fillColor', isShape,
        (n) => __bro_native.scene.SceneNode_fillColor_get(n),
        (n, v) => __bro_native.scene.SceneNode_fillColor_set(n, String(v)));
    typed('strokeColor', isShape,
        (n) => __bro_native.scene.SceneNode_strokeColor_get(n),
        (n, v) => __bro_native.scene.SceneNode_strokeColor_set(n, String(v)));
    typed('strokeWidth', isShape,
        (n) => __bro_native.scene.SceneNode_strokeWidth_get(n),
        (n, v) => __bro_native.scene.SceneNode_strokeWidth_set(n, +v));
    typed('autoSync', isPhysics,
        (n) => __bro_native.scene.SceneNode_autoSync_get(n),
        (n, v) => __bro_native.scene.SceneNode_autoSync_set(n, !!v));
    typed('pixelsPerUnit', isPhysics,
        (n) => __bro_native.scene.SceneNode_pixelsPerUnit_get(n),
        (n, v) => __bro_native.scene.SceneNode_pixelsPerUnit_set(n, +v));
    typed('frameIndex', isSprite,
        (n) => __bro_native.scene.SceneNode_frameIndex_get(n),
        (n, v) => __bro_native.scene.SceneNode_frameIndex_set(n, v | 0));
    const isShapeOrSprite = (node) => { const t = typeOf(node); return t === 'shape' || t === 'sprite'; };
    const isHtml = (node) => typeOf(node) === 'html';
    typed('anchorX', isShapeOrSprite,
        (n) => __bro_native.scene.SceneNode_anchorX_get(n),
        (n, v) => __bro_native.scene.SceneNode_anchor_set(n, +v, __bro_native.scene.SceneNode_anchorY_get(n)));
    typed('anchorY', isShapeOrSprite,
        (n) => __bro_native.scene.SceneNode_anchorY_get(n),
        (n, v) => __bro_native.scene.SceneNode_anchor_set(n, __bro_native.scene.SceneNode_anchorX_get(n), +v));
    typed('cornerRadius', isShape,
        (n) => __bro_native.scene.SceneNode_cornerRadius_get(n),
        (n, v) => __bro_native.scene.SceneNode_cornerRadius_set(n, +v));
    typed('pxPerUnit', isHtml,
        (n) => __bro_native.scene.SceneNode_pxPerUnit_get(n),
        (n, v) => __bro_native.scene.SceneNode_pxPerUnit_set(n, +v));
    typed('staticBatch', isInstanced,
        (n) => __bro_native.scene.SceneNode_staticBatch_get(n),
        (n, v) => __bro_native.scene.SceneNode_staticBatch_set(n, !!v));
    // Read-only: the atlas grid is set with setAtlasGrid / the atlasCols,
    // atlasRows or atlas create options.
    accessor(SceneNode.prototype, 'atlasCols', function () {
        return isInstanced(this) ? __bro_native.scene.SceneNode_atlasCols_get(this) : 0;
    }, undefined);
    accessor(SceneNode.prototype, 'atlasRows', function () {
        return isInstanced(this) ? __bro_native.scene.SceneNode_atlasRows_get(this) : 0;
    }, undefined);
    // The Jolt body id a physics node drives from; null with no body,
    // undefined on other node types.
    accessor(SceneNode.prototype, 'bodyId', function () {
        if (!isPhysics(this)) return undefined;
        const id = __bro_native.scene.SceneNode_bodyId_get(this);
        return id < 0 ? null : id;
    }, undefined);
    // billboard is a base-node field (drawn by Shape/Sprite/Html): 'full' | 'ylock'.
    accessor(SceneNode.prototype, 'billboard',
        function () { return __bro_native.scene.SceneNode_billboard_get(this); },
        function (v) { __bro_native.scene.SceneNode_billboard_set(this, String(v)); });

    fn(SceneNode.prototype, 'setAlphaCutoff', function setAlphaCutoff(v) { this.alphaCutoff = v; return this; });
    fn(SceneNode.prototype, 'setDoubleSided', function setDoubleSided(v) { this.doubleSided = v; return this; });
    fn(SceneNode.prototype, 'syncToPhysics', function syncToPhysics() {
        __bro_native.scene.SceneNode_syncToPhysics(this);
    });

    // ---- InstancedMesh operations -------------------------------------------
    fn(SceneNode.prototype, 'updateInstance', function updateInstance(index, data) {
        if (index === undefined || data === undefined) throw new TypeError('updateInstance: (index, 16 floats) are required');
        __bro_native.scene.SceneNode_updateInstance(this, index | 0, toF32(data));
    });
    // The whole-buffer spelling apps used beside updateInstance.
    fn(SceneNode.prototype, 'updateInstances', function updateInstances(data) {
        return this.setInstances(data);
    });
    fn(SceneNode.prototype, 'setInstancedMesh', function setInstancedMesh(mesh) {
        if (mesh === undefined || mesh === null) throw new TypeError('setInstancedMesh: mesh is required');
        __bro_native.scene.SceneNode_setInstancedMesh(this, mesh);
    });
    fn(SceneNode.prototype, 'setAtlasGrid', function setAtlasGrid(cols, rows) {
        __bro_native.scene.SceneNode_setAtlasGrid(this, cols | 0, rows | 0);
    });

    const SCATTER_KEYS = ['seed', 'upBias', 'tiltJitter', 'rollJitter', 'baseScale',
                          'scaleJitter', 'scaleByRadius', 'maxRadius', 'densityFalloff'];
    const SCATTER_DEFAULTS = [0, 0.5, 0.3, 0.2, 1.0, 0.2, 0.0, 0.05, 0.0];
    const boundsOf = (o) => {
        const mn = o.boundsMin, mx = o.boundsMax;
        if (mn && mx && mn.length >= 3 && mx.length >= 3) {
            return new Float32Array([+mn[0], +mn[1], +mn[2], +mx[0], +mx[1], +mx[2]]);
        }
        return EMPTY_F32;
    };
    // setScatterSegments({ segments, instSeg, seed, upBias, ..., boundsMin?, boundsMax? })
    // — the object world.emitScatterSegments(opts) produces.
    fn(SceneNode.prototype, 'setScatterSegments', function setScatterSegments(o) {
        if (!o || typeof o !== 'object') throw new TypeError('setScatterSegments: expected { segments, instSeg, ... }');
        const params = new Float64Array(SCATTER_KEYS.length);
        for (let i = 0; i < SCATTER_KEYS.length; i++) {
            const v = o[SCATTER_KEYS[i]];
            params[i] = typeof v === 'number' ? v : SCATTER_DEFAULTS[i];
        }
        __bro_native.scene.SceneNode_setScatterSegments(this, toF32(o.segments), toF32(o.instSeg), params, boundsOf(o));
    });
    // setTubeSegments({ segments, sides = 6, radiusScale = 1, boundsMin?, boundsMax? })
    // — the object world.emitBranchTubes(opts) produces.
    fn(SceneNode.prototype, 'setTubeSegments', function setTubeSegments(o) {
        if (!o || typeof o !== 'object') throw new TypeError('setTubeSegments: expected { segments, ... }');
        const sides = typeof o.sides === 'number' ? o.sides | 0 : 6;
        const radiusScale = typeof o.radiusScale === 'number' ? o.radiusScale : 1.0;
        __bro_native.scene.SceneNode_setTubeSegments(this, toF32(o.segments), sides, radiusScale, boundsOf(o));
    });
    accessor(SceneNode.prototype, 'isScatter', function () { return __bro_native.scene.SceneNode_isScatter(this); }, undefined);
    accessor(SceneNode.prototype, 'isTube', function () { return __bro_native.scene.SceneNode_isTube(this); }, undefined);

    // ---- factory option keys these accessors answer for ------------------------
    const applyExtras = (node, opts) => {
        if (!node || !opts) return node;
        if (opts.emissiveColor !== undefined) node.emissiveColor = opts.emissiveColor;
        if (opts.alphaCutoff !== undefined) node.alphaCutoff = opts.alphaCutoff;
        if (opts.doubleSided !== undefined) node.doubleSided = opts.doubleSided;
        if (opts.nearClipDist !== undefined) node.nearClipDist = opts.nearClipDist;
        if (opts.billboard !== undefined) node.billboard = opts.billboard;
        if (opts.radius !== undefined && isShape(node)) node.radius = opts.radius;
        if (opts.fillColor !== undefined && isShape(node)) node.fillColor = opts.fillColor;
        if (opts.strokeColor !== undefined && isShape(node)) node.strokeColor = opts.strokeColor;
        if (opts.strokeWidth !== undefined && isShape(node)) node.strokeWidth = opts.strokeWidth;
        if (opts.frameIndex !== undefined && isSprite(node)) node.frameIndex = opts.frameIndex;
        if (opts.autoSync !== undefined && isPhysics(node)) node.autoSync = opts.autoSync;
        if (opts.pixelsPerUnit !== undefined && isPhysics(node)) node.pixelsPerUnit = opts.pixelsPerUnit;
        if (isHtml(node)) {
            // createHtmlNode's native takes html/size/transform; the rest are
            // node attributes.
            if (opts.pxPerUnit !== undefined) node.pxPerUnit = opts.pxPerUnit;
            if (opts.worldAnchor !== undefined) node.worldAnchor = opts.worldAnchor;
            if (typeof opts.name === 'string') node.name = opts.name;
        }
        if (isInstanced(node)) {
            if (opts.atlas && typeof opts.atlas === 'object') {
                const cols = opts.atlas.cols !== undefined ? opts.atlas.cols : opts.atlas.columns;
                if (cols !== undefined && opts.atlas.rows !== undefined) node.setAtlasGrid(cols, opts.atlas.rows);
            }
            if (opts.scatter && typeof opts.scatter === 'object') node.setScatterSegments(opts.scatter);
            if (opts.tube && typeof opts.tube === 'object') node.setTubeSegments(opts.tube);
        }
        return node;
    };
    const wrapFactory = (name) => {
        const base = SceneGraph.prototype[name];
        if (typeof base !== 'function') return;
        fn(SceneGraph.prototype, name, function (opts) {
            return applyExtras(base.apply(this, arguments), opts);
        });
    };
    ['createMesh', 'createInstancedMesh', 'createShape', 'createSprite',
     'createPhysicsNode', 'createReflectionProbe', 'createHtmlNode'].forEach(wrapFactory);
})();
