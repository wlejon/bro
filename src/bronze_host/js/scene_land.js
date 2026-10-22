// scene_land.js — SceneGraph.createLand and the land node's members over
// native_scene_land.cpp.
//
// The node (scene/nodes/land_node.h) is a world's ground from underfoot to the
// horizon: its height a FastNoise node tree, run as GLSL (node.glsl()) where
// the engine makes the land's tiles and by FastNoise2 itself for the CPU's
// heights, and its surface the world's GLSL (scene_land_layer.glsl's
// landSurface). This file writes the module from the two and hands it over
// with the tree.
//
// Entered after js/scene_plants.js (installSceneModule, host_js_modules.cpp).
(function () {
    'use strict';

    const SceneNode = globalThis.SceneNode;
    const SceneGraph = globalThis.SceneGraph;
    if (typeof SceneNode !== 'function' || typeof SceneGraph !== 'function') return;
    const canon = __bro_native.scene.canonNode;

    const fn = (obj, name, value) =>
        Object.defineProperty(obj, name, { value, writable: true, enumerable: true, configurable: true });

    const PARAMS = ['extent', 'cellSize', 'resolution', 'levels'];

    // A surface of bare earth: what a land draws with no surface of its own.
    const BARE = `
LandSurface landSurface(LandPoint q) {
    LandSurface s;
    s.albedo = vec3(0.16, 0.13, 0.10);
    return s;
}
`;

    // The tree: the height's GLSL as landNoise and landTree over it at the
    // seed (in 2D for a flat body, in 3D at the datum's points for a round
    // one). The land's module and its processes both start with it.
    function treeOf(height, seed) {
        return height.glsl({ name: 'landNoise', dims: [2, 3] }) +
            '\nconst int LAND_SEED = ' + (seed | 0) + ';\n' +
            'float landTree(vec2 p) { return landNoise(p, LAND_SEED); }\n' +
            'float landTree(vec3 p) { return landNoise(p, LAND_SEED); }\n';
    }

    // The module: the tree, landHeight the processes' height over it
    // (landGrid's channel 0: x added, y the tree's scale; the tree alone
    // with none), then the surface (which may call any of them).
    const HEIGHT = (p) =>
        `float landHeight(${p} p) {\n` +
        '#ifdef LAND_GRID\n' +
        '    vec4 g = landGrid(0, p);\n' +
        '    return g.x + g.y * landTree(p);\n' +
        '#else\n' +
        '    return landTree(p);\n' +
        '#endif\n' +
        '}\n';
    function moduleOf(height, seed, surface) {
        const src = typeof surface === 'string' ? surface : (surface && surface.source) || BARE;
        return treeOf(height, seed) + HEIGHT('vec2') + HEIGHT('vec3') + src;
    }

    const isLand = (n) => n instanceof SceneNode && __bro_native.scene.SceneNode_landParams(n).length > 0;

    // Each land's module parts, for a later setLand that changes one.
    const parts = new WeakMap();

    function setProcesses(node, p) {
        const pr = p.processes;
        if (!pr) {
            const err = __bro_native.scene.SceneNode_setLandProcess(node, '', '', new Float64Array(0), new Float32Array(0));
            if (err) throw new TypeError('createLand: ' + err);
            return;
        }
        if (typeof pr.source !== 'string' || !pr.source)
            throw new TypeError('createLand: `processes.source` (GLSL defining procInit and the passes) is required');
        const passes = pr.passes || [];
        const params = [pr.resolution !== undefined ? +pr.resolution : 256, pr.channels !== undefined ? +pr.channels : 1];
        for (const s of passes) params.push(s.iterations !== undefined ? +s.iterations : 1);
        const err = __bro_native.scene.SceneNode_setLandProcess(node, treeOf(p.height, p.seed) + pr.source,
            passes.map((s) => String(s.fn)).join(','), new Float64Array(params),
            pr.data ? Float32Array.from(pr.data) : new Float32Array(0));
        if (err) throw new TypeError('createLand: ' + err);
    }

    function apply(node, o) {
        const was = parts.get(node) || { height: null, seed: 0, surface: null, data: null, processes: null };
        const tree = o.height !== undefined || o.seed !== undefined;
        if (tree || o.surface !== undefined || o.data !== undefined || o.processes !== undefined) {
            const p = {
                height: o.height !== undefined ? o.height : was.height,
                seed: o.seed !== undefined ? o.seed | 0 : was.seed,
                surface: o.surface !== undefined ? o.surface : was.surface,
                data: o.data !== undefined ? o.data : was.data,
                processes: o.processes !== undefined ? o.processes : was.processes,
            };
            if (!p.height) throw new TypeError('createLand: `height` (a FastNoise node) is what the land is');
            if (tree || o.surface !== undefined || o.data !== undefined) {
                const err = __bro_native.scene.SceneNode_setLandModule(node, moduleOf(p.height, p.seed, p.surface),
                    p.data ? Float32Array.from(p.data) : new Float32Array(0), p.height.encode(), p.seed);
                if (err) throw new TypeError('createLand: ' + err);
            }
            // (The processes read the tree: a new one runs them again.)
            if (o.processes !== undefined || (tree && p.processes)) setProcesses(node, p);
            parts.set(node, p);
        }
        const params = PARAMS.map((k) => (typeof o[k] === 'number' ? o[k] : NaN));
        if (params.some((v) => !Number.isNaN(v)))
            __bro_native.scene.SceneNode_setLandParams(node, new Float64Array(params));
        for (const k of ['name', 'castsShadow', 'receivesShadow', 'visible']) if (o[k] !== undefined) node[k] = o[k];
    }

    fn(SceneGraph.prototype, 'createLand', function createLand(opts) {
        if (!opts || !opts.height) throw new TypeError('createLand: `height` (a FastNoise node) is what the land is');
        const node = canon(__bro_native.scene.SceneGraph_createLand(this));
        if (node) apply(node, opts || {});
        return node;
    });

    const P = SceneNode.prototype;
    fn(P, 'setLand', function setLand(opts) {
        if (opts && isLand(this)) apply(this, opts);
        return this;
    });
    fn(P, 'landHeight', function landHeight(x, z) {
        return __bro_native.scene.SceneNode_landHeight(this, +x, +z);
    });
    fn(P, 'landParams', function landParams() {
        const a = __bro_native.scene.SceneNode_landParams(this);
        if (a.length < 4) return undefined;
        return { extent: a[0], cellSize: a[1], resolution: a[2], levels: a[3] };
    });
    fn(P, 'landStats', function landStats() {
        const a = __bro_native.scene.SceneNode_landStats(this);
        if (a.length < 8) return undefined;
        return { needed: a[0], resident: a[1], missing: a[2], filled: a[3], levels: a[4], shortfall: a[5],
                 compiled: a[6] > 0, ready: a[7] > 0 };
    });
})();
