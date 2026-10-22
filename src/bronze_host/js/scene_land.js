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

    // The module: the height's GLSL as landNoise, landHeight over it at the
    // seed (in 2D for a flat body, in 3D at the datum's points for a round
    // one), then the surface (which may call either).
    function moduleOf(height, seed, surface) {
        const noise = height.glsl({ name: 'landNoise', dims: [2, 3] });
        const src = typeof surface === 'string' ? surface : (surface && surface.source) || BARE;
        return noise + '\nconst int LAND_SEED = ' + (seed | 0) + ';\n' +
            'float landHeight(vec2 p) { return landNoise(p, LAND_SEED); }\n' +
            'float landHeight(vec3 p) { return landNoise(p, LAND_SEED); }\n' + src;
    }

    const isLand = (n) => n instanceof SceneNode && __bro_native.scene.SceneNode_landParams(n).length > 0;

    // Each land's module parts, for a later setLand that changes one.
    const parts = new WeakMap();

    function apply(node, o) {
        const was = parts.get(node) || { height: null, seed: 0, surface: null, data: null };
        if (o.height !== undefined || o.seed !== undefined || o.surface !== undefined || o.data !== undefined) {
            const p = {
                height: o.height !== undefined ? o.height : was.height,
                seed: o.seed !== undefined ? o.seed | 0 : was.seed,
                surface: o.surface !== undefined ? o.surface : was.surface,
                data: o.data !== undefined ? o.data : was.data,
            };
            if (!p.height) throw new TypeError('createLand: `height` (a FastNoise node) is what the land is');
            const err = __bro_native.scene.SceneNode_setLandModule(node, moduleOf(p.height, p.seed, p.surface),
                p.data ? Float32Array.from(p.data) : new Float32Array(0), p.height.encode(), p.seed);
            if (err) throw new TypeError('createLand: ' + err);
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
        if (a.length < 7) return undefined;
        return { needed: a[0], resident: a[1], missing: a[2], filled: a[3], levels: a[4], shortfall: a[5],
                 compiled: a[6] > 0 };
    });
})();
