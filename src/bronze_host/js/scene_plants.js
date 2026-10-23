// scene_plants.js — SceneGraph.createPlants and the plants node's members
// over native_scene_plants.cpp.
//
// The node (scene/nodes/plants_node.h) builds every plant on the GPU from a table
// of species, and plants placed about the eye by rules (FastNoise fields) or set
// by hand; this file packs species, plant and rule objects into what the node
// takes.
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
    const SPECIES_ENGINE_FLOATS = 12;
    const PLANT_FLOATS = 8;
    // A species' engine fields in scene_plant_records.glsl's PlantSpecies
    // order, with the defaults (a plant about 12 m tall). Colours take three
    // slots. What the plant looks like is its `data` (the module's plantData).
    const SPECIES = [
        ['height', 1, 12], ['base', 1, 0.3], ['radius', 1, 4], ['elements', 1, 6000],
        ['leafLength', 1, 0.22], ['limbs', 1, 18], ['shape', 1, 0], ['density', 1, 0.6],
        ['albedo', 3, [0.18, 0.27, 0.07]], [null, 1, 0],
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
                    out[at] = key && typeof s[key] === 'number' ? s[key] : def;
                }
                at += n;
            }
            const data = s.data || [];
            const room = SPECIES_FLOATS - SPECIES_ENGINE_FLOATS;
            if (data.length > room) throw new RangeError('plants: a species\' data is at most ' + room + ' numbers');
            for (let k = 0; k < data.length; ++k) out[i * SPECIES_FLOATS + SPECIES_ENGINE_FLOATS + k] = +data[k] || 0;
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

    // Rules as world/plant_placement.h's JSON: every field (a FastNoise node,
    // its encoded tree, a number for a constant, or { tree, seed } of any of
    // them) once in a table, each rule's field its index there.
    function rulesJson(r) {
        const fields = [];
        const known = new Map();
        const encode = (tree) => {
            if (typeof tree === 'string') return tree;
            if (typeof tree === 'number') {
                const c = FastNoise.create('Constant');
                c.set('Value', tree);
                return c.encode();
            }
            return tree.encode();
        };
        const field = (f) => {
            if (f === undefined || f === null) return null;
            const tree = f.tree !== undefined ? f.tree : f;
            const seed = typeof f.seed === 'number' ? f.seed | 0 : (typeof r.seed === 'number' ? r.seed | 0 : 1337);
            const encoded = encode(tree);
            const key = encoded + '#' + seed;
            if (!known.has(key)) { known.set(key, fields.length); fields.push({ tree: encoded, seed }); }
            return known.get(key);
        };
        const num = (v) => (typeof v === 'number' ? v : undefined);
        // A layer's value: a field, or the land's own value ({ own: 'z' | 'w',
        // scale, offset, times: a field it multiplies }).
        const term = (t) => {
            if (!t || typeof t !== 'object' || t.own === undefined) return field(t);
            if (t.own !== 'z' && t.own !== 'w') throw new TypeError("createPlants: a rule's `own` is 'z' or 'w'");
            return { own: t.own === 'z' ? 0 : 1, scale: num(t.scale), offset: num(t.offset), field: field(t.times) };
        };
        const layers = (r.layers || []).map((l, i) => ({
            spacing: num(l.spacing), jitter: num(l.jitter), chance: term(l.chance),
            species: (l.species || []).map((s) => (typeof s === 'number' ? { species: s, weight: null }
                                                                     : { species: s.species | 0, weight: term(s.weight) })),
            scale: term(l.scale), scaleVariety: num(l.scaleVariety),
            tint: term(l.tint), tintVariety: num(l.tintVariety),
            sink: num(l.sink), seed: typeof l.seed === 'number' ? l.seed : i + 1,
        }));
        return JSON.stringify({ fields, ground: field(r.ground), range: num(r.range), hysteresis: num(r.hysteresis), layers });
    }

    const isPlants = (n) => n instanceof SceneNode && __bro_native.scene.SceneNode_plantStats(n).length > 0;

    // The land each plants node stands on: it takes their canopy (its
    // rules', species' and ranges') whenever one of them changes.
    const lands = new WeakMap();

    function apply(node, o) {
        if (o.module !== undefined) {
            const source = typeof o.module === 'string' ? o.module : (o.module && o.module.source) || '';
            __bro_native.scene.SceneNode_setPlantModule(node, source);
        }
        if (o.species) __bro_native.scene.SceneNode_setPlantSpecies(node, speciesRecords(o.species));
        if (o.plants) __bro_native.scene.SceneNode_setPlants(node, plantRecords(o.plants));
        if (o.rules !== undefined) {
            const err = __bro_native.scene.SceneNode_setPlantRules(node, o.rules ? rulesJson(o.rules) : '');
            if (err) throw new TypeError(err);
        }
        const params = PARAMS.map((k) => (typeof o[k] === 'number' ? o[k] : NaN));
        if (params.some((v) => !Number.isNaN(v)))
            __bro_native.scene.SceneNode_setPlantParams(node, new Float64Array(params));
        for (const k of ['name', 'castsShadow', 'receivesShadow', 'visible']) if (o[k] !== undefined) node[k] = o[k];
        if (o.land !== undefined) {
            if (!(o.land instanceof SceneNode) && o.land !== null) throw new TypeError('plants: `land` is a land node');
            const was = lands.get(node);
            if (was && was !== o.land) __bro_native.scene.SceneNode_setPlantLand(node, was, false);
            if (o.land) lands.set(node, o.land); else lands.delete(node);
        }
        const land = lands.get(node);
        if (land && (o.land !== undefined || o.rules !== undefined || o.species || params.some((v) => !Number.isNaN(v)))) {
            const err = __bro_native.scene.SceneNode_setPlantLand(node, land, true);
            if (err) throw new TypeError('plants: ' + err);
        }
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
    fn(P, 'plantPlacement', function plantPlacement() {
        const a = __bro_native.scene.SceneNode_plantPlacement(this);
        if (a.length < 2) return undefined;
        return { cells: a[0], pending: a[1] };
    });
    fn(P, 'plantStats', function plantStats() {
        const a = __bro_native.scene.SceneNode_plantStats(this);
        if (a.length < 4) return undefined;
        return { plants: a[0], visible: a[1], elements: a[2], limbs: a[3], shadowElements: a[4] };
    });
})();
