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
    // Then the regional levels' relief (processes.regions.relief, a tree a
    // level): landRelief(L, p), each tree written once, LAND_RELIEF_LEVELS of
    // them.
    function treeOf(height, seed, relief) {
        let src = height.glsl({ name: 'landNoise', dims: [2, 3] }) +
            '\nconst int LAND_SEED = ' + (seed | 0) + ';\n' +
            'float landTree(vec2 p) { return landNoise(p, LAND_SEED); }\n' +
            'float landTree(vec3 p) { return landNoise(p, LAND_SEED); }\n';
        const rl = relief || [];
        for (let L = 0; L < rl.length; ++L) src += rl[L].noise.glsl({ name: 'landReliefN' + L, dims: [2, 3] });
        src += '\nconst int LAND_RELIEF_LEVELS = ' + rl.length + ';\n';
        for (const t of ['vec2', 'vec3']) {
            src += `float landRelief(int L, ${t} p) {\n`;
            for (let L = 0; L < rl.length; ++L) src += `    if (L == ${L}) return landReliefN${L}(p, ${rl[L].seed | 0});\n`;
            src += '    return 0.0;\n}\n';
        }
        return src;
    }

    // processes.regions.relief as [{ noise, seed }], a level each from level 0
    // (a bare FastNoise node takes the land's seed).
    function reliefOf(processes, seed) {
        const r = processes && processes.regions && processes.regions.relief;
        if (!r) return [];
        if (!Array.isArray(r)) throw new TypeError('createLand: `processes.regions.relief` is an array, a level\'s tree each');
        return r.map((e, L) => {
            const noise = e && typeof e.glsl === 'function' ? e : e && e.noise;
            if (!noise || typeof noise.glsl !== 'function')
                throw new TypeError(`createLand: \`processes.regions.relief[${L}]\` is a FastNoise node or { noise, seed }`);
            return { noise, seed: e.noise && e.seed !== undefined ? e.seed | 0 : seed | 0 };
        });
    }

    // The module: the tree, landHeight the processes' height over it
    // (landGrid's channel 0: x added, y the tree's scale, by which the tree
    // and each regional level's relief where that level's regions are not
    // in; the tree alone with none), then the surface (which may call any
    // of them).
    const HEIGHT = (p, relief) =>
        `float landHeight(${p} p) {\n` +
        '#ifdef LAND_GRID\n' +
        '    vec4 g = landGrid(0, p);\n' +
        '    float t = landTree(p);\n' +
        (relief ?
            '    for (int L = 0; L < LAND_LOOP_N(LAND_RELIEF_LEVELS); ++L) {\n' +
            '        float r = landReliefRest(L);\n' +
            '        if (r > 0.0) t += r * landRelief(L, p);\n' +
            '    }\n' : '') +
        '    return g.x + g.y * t;\n' +
        '#else\n' +
        '    return landTree(p);\n' +
        '#endif\n' +
        '}\n';
    function moduleOf(height, seed, surface, relief) {
        const src = typeof surface === 'string' ? surface : (surface && surface.source) || BARE;
        const any = relief.length > 0;
        return treeOf(height, seed, relief) + HEIGHT('vec2', any) + HEIGHT('vec3', any) + src;
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
        let names = passes.map((s) => String(s.fn)).join(',');
        // Its regional grids (the same source's kernels, over regions under the eye).
        const rg = pr.regions;
        if (rg) {
            if (typeof rg.init !== 'string' || !rg.init)
                throw new TypeError('createLand: `processes.regions.init` (the regions\' init kernel) is required');
            const rp = rg.passes || [];
            const num = (v, d) => (v !== undefined ? +v : d);
            // (What a region conserves: 'c.x' channel c's component, as bits c * 4 + 0 .. 3.)
            let conserve = 1;
            if (rg.conserve !== undefined) {
                conserve = 0;
                for (const s of rg.conserve) {
                    const m = /^(\d)\.([xyzw])$/.exec(String(s));
                    if (!m) throw new TypeError(`createLand: \`processes.regions.conserve\`: '${s}' is not a channel's component ('0.x')`);
                    conserve |= 1 << (+m[1] * 4 + 'xyzw'.indexOf(m[2]));
                }
            }
            params.push(num(rg.cells, 512), num(rg.apron, 32), num(rg.cell, 64), num(rg.ratio, 4), num(rg.levels, 3),
                num(rg.reach, 1), num(rg.keep, 2), conserve);
            for (const s of rp) params.push(s.iterations !== undefined ? +s.iterations : 1);
            names += '|' + String(rg.init) + '|' + rp.map((s) => String(s.fn)).join(',');
        }
        const err = __bro_native.scene.SceneNode_setLandProcess(node, treeOf(p.height, p.seed, p.relief) + pr.source,
            names, new Float64Array(params),
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
            // (The regional levels' relief: part of the module's height and the CPU's.)
            p.relief = reliefOf(p.processes, p.seed);
            p.reliefKey = p.relief.map((r) => r.noise.encode() + ':' + r.seed).join('\n');
            const relief = p.reliefKey !== (was.reliefKey || '');
            if (tree || o.surface !== undefined || o.data !== undefined || relief) {
                const err = __bro_native.scene.SceneNode_setLandModule(node, moduleOf(p.height, p.seed, p.surface, p.relief),
                    p.data ? Float32Array.from(p.data) : new Float32Array(0), p.height.encode(), p.seed);
                if (err) throw new TypeError('createLand: ' + err);
            }
            if (relief) {
                const err = __bro_native.scene.SceneNode_setLandRelief(node, p.relief.map((r) => r.noise.encode()).join('\n'),
                    new Float64Array(p.relief.map((r) => r.seed)));
                if (err) throw new TypeError('createLand: ' + err);
            }
            // (The processes read the tree: a new one runs them again.)
            if (o.processes !== undefined || (tree && p.processes)) setProcesses(node, p);
            parts.set(node, p);
        }
        const params = PARAMS.map((k) => (typeof o[k] === 'number' ? o[k] : NaN));
        // The sea after them: on, level, absorb rgb, scatter rgb, roughness
        // (NaN: as it was).
        if (o.sea !== undefined) {
            const s = o.sea || {};
            const rgb = (v) => (v && v.length >= 3 ? [+v[0], +v[1], +v[2]] : [NaN, NaN, NaN]);
            const num = (v) => (typeof v === 'number' ? v : NaN);
            params.push(o.sea ? 1 : 0, num(s.level), ...rgb(s.absorb), ...rgb(s.scatter), num(s.roughness));
        }
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
                 compiled: a[6] > 0, ready: a[7] > 0, regions: a[8], regionsPending: a[9], groundReady: a[10] > 0 };
    });
})();
