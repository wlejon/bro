// scene_forest.js — SceneGraph.createForest and the forest node's members over
// native_scene_forest.cpp.
//
// The node (scene/forest_node.h) draws many trees from a few grown plants:
// bro.flora's world.emitForest() gives the specimens (each plant's bark,
// clumps and leaves relative to where it stands), bro.flora.plantForest()
// or the emit's own `instances` say where each tree stands. The node picks
// every tree's level of detail each frame from its size on screen.
//
// Entered after js/scene_grass.js (installSceneModule, host_js_modules.cpp).
(function () {
    'use strict';

    const SceneNode = globalThis.SceneNode;
    const SceneGraph = globalThis.SceneGraph;
    if (typeof SceneNode !== 'function' || typeof SceneGraph !== 'function') return;
    const canon = __bro_native.scene.canonNode;

    const fn = (obj, name, value) =>
        Object.defineProperty(obj, name, { value, writable: true, enumerable: true, configurable: true });

    // ForestNode::lodFromArray's order.
    const LOD = ['leafPixels', 'flutterFade', 'cardWidening', 'clumpFrom', 'clumpTo', 'crownFrom', 'crownTo',
                 'barkPixels', 'barkSidesPixels', 'shadowKeep', 'range', 'budget', 'shadowBudget'];
    const TIERS = { auto: -1, leaves: 0, clumps: 1, crowns: 2 };

    const isForest = (node) => node instanceof SceneNode && __bro_native.scene.SceneNode_forestLod(node).length === LOD.length;

    function parseColor(c) {
        if (typeof c === 'string' && c[0] === '#') {
            if (c.length === 7) return [parseInt(c.slice(1, 3), 16) / 255, parseInt(c.slice(3, 5), 16) / 255, parseInt(c.slice(5, 7), 16) / 255];
            if (c.length === 4) return [parseInt(c[1] + c[1], 16) / 255, parseInt(c[2] + c[2], 16) / 255, parseInt(c[3] + c[3], 16) / 255];
        }
        if (c && typeof c.length === 'number' && c.length >= 3) return [+c[0], +c[1], +c[2]];
        return null;
    }

    const f32 = (a) => a instanceof Float32Array ? a : new Float32Array(a || []);

    function setSpecimens(node, forest) {
        if (!forest) return false;
        const l = forest.leaves;
        const leaves = l instanceof Uint32Array || l instanceof Int32Array
            ? new Int32Array(l.buffer, l.byteOffset, l.length) : new Int32Array(l || []);
        const leaf = new Float64Array([+forest.seed || 0, +forest.upBias || 0, +forest.tiltJitter || 0,
                                       +forest.shell || 0, forest.baseScale === undefined ? 1 : +forest.baseScale,
                                       +forest.scaleJitter || 0]);
        return __bro_native.scene.SceneNode_setForestSpecimens(node, f32(forest.specimens), f32(forest.segments),
            f32(forest.tangents), f32(forest.segmentVegetation), f32(forest.keepRadius), f32(forest.clumps),
            f32(forest.clumpVegetation), leaves, leaf);
    }

    function setLod(node, lod) {
        const a = new Float64Array(LOD.length).fill(NaN);
        LOD.forEach((k, i) => { if (typeof lod[k] === 'number') a[i] = lod[k]; });
        __bro_native.scene.SceneNode_setForestLod(node, a);
    }

    function applyOpts(node, o) {
        if (o.forest && !setSpecimens(node, o.forest)) throw new TypeError('createForest: the forest arrays do not fit together');
        const inst = o.instances || (o.forest && o.instances === undefined ? o.forest.instances : null);
        if (inst) __bro_native.scene.SceneNode_setForestInstances(node, f32(inst));
        if (o.lod) setLod(node, o.lod);
        if (o.barkColor !== undefined || o.barkRoughness !== undefined) {
            const c = parseColor(o.barkColor) || [0.2, 0.17, 0.14];
            __bro_native.scene.SceneNode_setForestBark(node, c[0], c[1], c[2],
                typeof o.barkRoughness === 'number' ? o.barkRoughness : 0.9);
        }
        if (o.tier !== undefined) node.setForestTier(o.tier);
        for (const k of ['name', 'x', 'y', 'z', 'position', 'rotation', 'scale', 'visible']) if (o[k] !== undefined) node[k] = o[k];
    }

    fn(SceneGraph.prototype, 'createForest', function createForest(opts) {
        const o = opts || {};
        const leaf = o.leaf && typeof o.leaf !== 'string' ? o.leaf : (o.mesh && typeof o.mesh !== 'string' ? o.mesh : null);
        const node = canon(__bro_native.scene.SceneGraph_createForest(this, o, leaf));
        if (!node) return node;
        applyOpts(node, o);
        return node;
    });

    const P = SceneNode.prototype;
    fn(P, 'setForest', function setForest(opts) {
        if (isForest(this) && opts) applyOpts(this, opts);
        return this;
    });
    fn(P, 'setTrees', function setTrees(instances) {
        if (isForest(this)) __bro_native.scene.SceneNode_setForestInstances(this, f32(instances));
        return this;
    });
    fn(P, 'forestLod', function forestLod() {
        const a = __bro_native.scene.SceneNode_forestLod(this);
        if (a.length !== LOD.length) return undefined;
        const o = {};
        LOD.forEach((k, i) => { o[k] = a[i]; });
        return o;
    });
    fn(P, 'setForestLod', function setForestLod(lod) {
        if (isForest(this) && lod) setLod(this, lod);
        return this;
    });
    fn(P, 'setForestTier', function setForestTier(tier) {
        const t = typeof tier === 'number' ? tier : TIERS[tier] !== undefined ? TIERS[tier] : -1;
        __bro_native.scene.SceneNode_setForestTier(this, t | 0);
        return this;
    });
    fn(P, 'forestStats', function forestStats() {
        const s = __bro_native.scene.SceneNode_forestStats(this);
        if (s.length < 13) return undefined;
        return { trees: { leaves: s[0], clumps: s[1], crowns: s[2] }, culled: s[3], leaves: s[4], vertices: s[5],
                 shadowVertices: s[6], entries: s[7], draws: s[8], treeCount: s[9], specimenCount: s[10],
                 detail: s[11], shadowDetail: s[12] };
    });
    // What specimen `specimen` draws at `pxPerUnit` pixels per specimen unit
    // (the view's pixels per metre at the tree, times its scale).
    fn(P, 'forestTreeLod', function forestTreeLod(specimen, pxPerUnit) {
        const a = __bro_native.scene.SceneNode_forestTreeLod(this, specimen | 0, +pxPerUnit);
        if (a.length < 8) return undefined;
        return { widening: a[0], keep: a[1], card: a[2] > 0, leaves: a[3], clumps: a[4], crowns: a[5],
                 barkRadius: a[6], barkSides: a[7] };
    });
})();
