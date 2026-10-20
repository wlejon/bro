// scene_plants.js — SceneGraph.createPlants and the plants node's members
// over native_scene_plants.cpp.
//
// The node (scene/nodes/plants_node.h) builds every plant on the GPU from a table
// of species and a list of plants; this file packs species objects and plant
// objects into the records the node takes.
//
// Entered after js/scene_extras.js (installSceneModule, host_js_modules.cpp).
(function () {
    'use strict';

    const SceneNode = globalThis.SceneNode;
    const SceneGraph = globalThis.SceneGraph;
    if (typeof SceneNode !== 'function' || typeof SceneGraph !== 'function') return;
    const canon = __bro_native.scene.canonNode;

    const fn = (obj, name, value) =>
        Object.defineProperty(obj, name, { value, writable: true, enumerable: true, configurable: true });

    const SPECIES_FLOATS = 32;
    const PLANT_FLOATS = 8;
    // A species' fields in scene_plants.glsl's PlantSpecies order, with the
    // defaults (a broadleaf about 12 m tall). Colours take three slots.
    const SPECIES = [
        ['height', 1, 12], ['base', 1, 0.3], ['radius', 1, 4], ['shape', 1, 0],
        ['lobes', 1, 18], ['lobeSize', 1, 0.42], ['leafLength', 1, 0.22], ['leafWidth', 1, 0.5],
        ['elements', 1, 6000], ['trunk', 1, 0.25], ['spread', 1, 0.8], ['droop', 1, 0.2],
        ['light', 3, [0.32, 0.45, 0.10]], ['hueVariety', 1, 0.25],
        ['dark', 3, [0.05, 0.10, 0.04]], ['glow', 1, 0.8],
        ['bark', 3, [0.16, 0.12, 0.09]], ['density', 1, 0.6],
        ['glowColor', 3, [1.0, 0.95, 0.4]], ['needle', 1, 0],
    ];
    const PARAMS = ['pixels', 'minElements', 'woodRange', 'range', 'shadowTexels', 'shadowRange'];

    function parseColor(c) {
        if (typeof c === 'string' && c[0] === '#') {
            const h = c.length === 4 ? c[1] + c[1] + c[2] + c[2] + c[3] + c[3] : c.slice(1, 7);
            // '#rrggbb' is sRGB: the records are linear.
            const lin = (v) => { v /= 255; return v <= 0.04045 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4); };
            return [lin(parseInt(h.slice(0, 2), 16)), lin(parseInt(h.slice(2, 4), 16)), lin(parseInt(h.slice(4, 6), 16))];
        }
        if (c && typeof c.length === 'number' && c.length >= 3) return [+c[0], +c[1], +c[2]];
        return null;
    }

    function speciesRecords(list) {
        if (list instanceof Float32Array) return list;
        const out = new Float32Array(list.length * SPECIES_FLOATS);
        for (let i = 0; i < list.length; ++i) {
            const s = list[i] || {};
            let at = i * SPECIES_FLOATS;
            for (const [key, n, def] of SPECIES) {
                if (n === 3) {
                    const c = parseColor(s[key]) || def;
                    out[at] = c[0]; out[at + 1] = c[1]; out[at + 2] = c[2];
                } else {
                    out[at] = typeof s[key] === 'number' ? s[key] : def;
                }
                at += n;
            }
        }
        return out;
    }

    function plantRecords(list) {
        if (list instanceof Float32Array) return list;
        const out = new Float32Array(list.length * PLANT_FLOATS);
        for (let i = 0; i < list.length; ++i) {
            const p = list[i];
            const at = i * PLANT_FLOATS;
            out[at] = +p.x || 0; out[at + 1] = +p.y || 0; out[at + 2] = +p.z || 0;
            out[at + 3] = +p.yaw || 0;
            out[at + 4] = typeof p.scale === 'number' ? p.scale : 1;
            out[at + 5] = p.species | 0;
            out[at + 6] = typeof p.seed === 'number' ? (p.seed >>> 0) % 16777216 : i;
            out[at + 7] = +p.tint || 0;
        }
        return out;
    }

    const isPlants = (n) => n instanceof SceneNode && __bro_native.scene.SceneNode_plantStats(n).length > 0;

    function apply(node, o) {
        if (o.species) __bro_native.scene.SceneNode_setPlantSpecies(node, speciesRecords(o.species));
        if (o.plants) __bro_native.scene.SceneNode_setPlants(node, plantRecords(o.plants));
        const params = PARAMS.map((k) => (typeof o[k] === 'number' ? o[k] : NaN));
        if (params.some((v) => !Number.isNaN(v)))
            __bro_native.scene.SceneNode_setPlantParams(node, new Float64Array(params));
        for (const k of ['name', 'castsShadow', 'receivesShadow', 'visible']) if (o[k] !== undefined) node[k] = o[k];
    }

    fn(SceneGraph.prototype, 'createPlants', function createPlants(opts) {
        const node = canon(__bro_native.scene.SceneGraph_createPlants(this));
        if (node) apply(node, opts || {});
        return node;
    });

    const P = SceneNode.prototype;
    fn(P, 'setPlants', function setPlants(opts) {
        if (opts && isPlants(this)) apply(this, Array.isArray(opts) || opts instanceof Float32Array ? { plants: opts } : opts);
        return this;
    });
    fn(P, 'plantStats', function plantStats() {
        const a = __bro_native.scene.SceneNode_plantStats(this);
        if (a.length < 4) return undefined;
        return { plants: a[0], visible: a[1], elements: a[2], limbs: a[3], shadowElements: a[4] };
    });
})();
