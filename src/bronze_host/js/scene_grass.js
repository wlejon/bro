// scene_grass.js — SceneGraph.createGrass and the grass node's members over
// native_scene_grass.cpp.
//
// The node (scene/nodes/grass_node.h) draws blades over a FIELD: a grid of (ground
// height, density, dryness, height multiplier). This file bakes that field
// from whatever the app's ground is (a mesh node, a terrain, a clipmap, a
// height function) and from the app's density / dryness / height inputs
// (numbers or functions of x, z, y), then hands it over whole.
//
// Entered after js/scene_extras.js (installSceneModule, host_js_modules.cpp).
(function () {
    'use strict';

    const SceneNode = globalThis.SceneNode;
    const SceneGraph = globalThis.SceneGraph;
    if (typeof SceneNode !== 'function' || typeof SceneGraph !== 'function') return;
    // Natives are spelled out in full at each call: the compiler binds
    // `__bro_native.<path>(...)` to a direct call, not a call through an alias.
    const canon = __bro_native.scene.canonNode;

    const fn = (obj, name, value) =>
        Object.defineProperty(obj, name, { value, writable: true, enumerable: true, configurable: true });

    // The params, in GrassNode::paramsFromArray's order. What the blades are
    // (their shape, colour, where they stand) is the world's sward module.
    const PARAMS = ['width', 'density', 'range'];
    const PARAM_COUNT = 3;

    // opts -> the flat list, NaN where opts says nothing.
    function paramArray(opts) {
        const a = new Float64Array(PARAM_COUNT).fill(NaN);
        PARAMS.forEach((name, i) => { if (typeof opts[name] === 'number') a[i] = opts[name]; });
        return a;
    }

    const isGrass = (node) => node instanceof SceneNode && __bro_native.scene.SceneNode_grassParams(node).length === PARAM_COUNT;

    // The field each grass node was baked with, for heightAt.
    const fields = new WeakMap();

    // A height source: (x, z) => y, or a whole grid at once for a mesh node.
    function heightSource(ground) {
        if (typeof ground === 'function') return { at: ground };
        if (ground && typeof ground.renderedElevationAt === 'function')
            return { at: (x, z) => ground.renderedElevationAt(x, z) };
        if (ground && typeof ground.heightAt === 'function' && !(ground instanceof SceneNode))
            return { at: (x, z) => ground.heightAt(x, z) };
        if (ground instanceof SceneNode) return { node: ground };
        return { at: () => 0 };
    }

    function areaOf(opts, src) {
        const a = opts.area;
        if (a && a.length >= 4) return [+a[0], +a[1], +a[2], +a[3]];
        if (src.node) {
            const b = __bro_native.scene.SceneNode_meshWorldBounds(src.node);
            if (b.length === 6) return [b[0], b[2], b[3], b[5]];
        }
        return [-50, -50, 50, 50];
    }

    const valueAt = (v, dflt) => {
        if (typeof v === 'function') return v;
        const k = typeof v === 'number' ? v : dflt;
        return () => k;
    };
    // An input over the whole field at once, or null (asked a texel at a
    // time): a Float32Array of cols x rows on the field's grid, or what a
    // function's `grid(x0, z0, cell, cols, rows, ys, canopy)` returns (ys
    // the field's heights, canopy the floor's when there is one).
    const gridOf = (v, x0, z0, cell, cols, rows, ys, canopy) => {
        let g = null;
        if (v instanceof Float32Array) g = v;
        else if (typeof v === 'function' && typeof v.grid === 'function') g = v.grid(x0, z0, cell, cols, rows, ys, canopy);
        if (!g) return null;
        if (!(g instanceof Float32Array) || g.length !== cols * rows)
            throw new TypeError(`createGrass: a grid input must be a Float32Array of ${cols} x ${rows}`);
        return g;
    };

    // Bake the field: heights from the ground, then density / dry / height
    // per texel. Texels with no ground (off a mesh) get no grass.
    function bake(node, opts) {
        const src = heightSource(opts.ground);
        const area = areaOf(opts, src);
        const cell = opts.cell > 0 ? +opts.cell : 0.5;
        const cols = Math.max(2, Math.floor((area[2] - area[0]) / cell) + 1);
        const rows = Math.max(2, Math.floor((area[3] - area[1]) / cell) + 1);
        const x0 = area[0], z0 = area[1];
        let heights;
        if (src.node) {
            heights = __bro_native.scene.SceneNode_sampleHeights(src.node, x0, z0, cell, cols, rows);
            if (heights.length !== cols * rows) heights = new Float32Array(cols * rows).fill(NaN);
        } else if (typeof opts.ground === 'function' && typeof opts.ground.grid === 'function') {
            heights = gridOf(opts.ground, x0, z0, cell, cols, rows);
        } else {
            heights = new Float32Array(cols * rows);
            for (let j = 0; j < rows; j++)
                for (let i = 0; i < cols; i++) heights[j * cols + i] = +src.at(x0 + i * cell, z0 + j * cell);
        }
        const n = cols * rows;
        // The heights, a texel off the ground taking the last one on it in
        // its row (a row that starts off the mesh, its first from the right).
        // These loops run a texel at a time over millions of texels: the
        // clamps are written out and `v - v === 0` is "v is finite" (a call
        // to Math.min / Number.isFinite costs this AOT code ~100 ns).
        const ys = new Float32Array(n);
        for (let j = 0; j < rows; j++) {
            let last = 0, seen = false, first = -1;
            for (let i = 0; i < cols; i++) {
                const k = j * cols + i, y = heights[k];
                if (y - y === 0) { last = y; seen = true; if (first < 0) first = i; }
                ys[k] = y - y === 0 ? y : seen ? last : 0;
            }
            for (let i = 0; i < first; i++) ys[j * cols + i] = ys[j * cols + first];
        }
        const density = valueAt(opts.density, 1), dry = valueAt(opts.dry, 0), tall = valueAt(opts.tall, 1);
        const gDensity = gridOf(opts.density, x0, z0, cell, cols, rows, ys);
        const gDry = gridOf(opts.dry, x0, z0, cell, cols, rows, ys);
        const gTall = gridOf(opts.tall, x0, z0, cell, cols, rows, ys);
        const data = new Float32Array(n * 4);
        for (let j = 0; j < rows; j++) {
            for (let i = 0; i < cols; i++) {
                const k = j * cols + i;
                const x = x0 + i * cell, z = z0 + j * cell, y = ys[k], h = heights[k];
                data[k * 4] = y;
                if (h - h === 0) {
                    const d = gDensity ? gDensity[k] : +density(x, z, y);
                    const w = gDry ? gDry[k] : +dry(x, z, y);
                    const t = gTall ? gTall[k] : +tall(x, z, y);
                    data[k * 4 + 1] = d < 0 ? 0 : d > 1 ? 1 : d;
                    data[k * 4 + 2] = w < 0 ? 0 : w > 1 ? 1 : w;
                    data[k * 4 + 3] = t < 0 ? 0 : t;
                }
            }
        }
        __bro_native.scene.SceneNode_setGrassField(node, data, cols, rows, x0, z0, cell);
        fields.set(node, { data, cols, rows, x0, z0, cell });
        // The forest floor, on the same grid: how closed the canopy is, and
        // with herbs how thickly they grow (a function of x, z, y and that
        // canopy, or a number).
        const herbs = opts.herbs;
        const hasFloor = opts.floor !== undefined && opts.floor !== null;
        if (hasFloor || herbs) {
            const floorAt = valueAt(hasFloor ? opts.floor : 0, 0);
            const herbAt = herbs ? valueAt(herbs.density, 1) : null;
            const gFloor = hasFloor ? gridOf(opts.floor, x0, z0, cell, cols, rows, ys) : null;
            const ch = herbs ? 2 : 1;
            const fl = new Float32Array(n * ch);
            for (let j = 0; j < rows; j++)
                for (let i = 0; i < cols; i++) {
                    const k = j * cols + i;
                    const v = gFloor ? gFloor[k] : +floorAt(x0 + i * cell, z0 + j * cell, ys[k]);
                    fl[k * ch] = v > 0 ? (v > 1 ? 1 : v) : 0;
                }
            if (herbAt) {
                // The herbs' density sees the floor's canopy, as clamped.
                let canopy = null;
                if (typeof herbs.density === 'function' && typeof herbs.density.grid === 'function') {
                    canopy = new Float32Array(n);
                    for (let k = 0; k < n; k++) canopy[k] = fl[k * ch];
                }
                const gHerb = gridOf(herbs.density, x0, z0, cell, cols, rows, ys, canopy);
                for (let j = 0; j < rows; j++)
                    for (let i = 0; i < cols; i++) {
                        const k = j * cols + i;
                        const v = gHerb ? gHerb[k] : +herbAt(x0 + i * cell, z0 + j * cell, ys[k], fl[k * ch]);
                        fl[k * ch + 1] = v > 0 ? (v > 1 ? 1 : v) : 0;
                    }
            }
            __bro_native.scene.SceneNode_setGrassFloor(node, fl);
        }
        if (herbs) setHerbs(node, herbs);
    }

    // The herbs: a world's herb module (GLSL, docs/vegetation-api.js
    // "Herbs"), the data its herbData(i) reads, and its tallest plant.
    function setHerbs(node, herbs) {
        const data = herbs.data instanceof Float32Array ? herbs.data
            : new Float32Array(Array.isArray(herbs.data) ? herbs.data : 0);
        __bro_native.scene.SceneNode_setGrassHerbs(node, typeof herbs.source === 'string' ? herbs.source : '', data,
            typeof herbs.height === 'number' ? herbs.height : 0.4,
            typeof herbs.range === 'number' ? herbs.range : 0, typeof herbs.far === 'number' ? herbs.far : 0);
    }

    // The blades: a world's sward module (GLSL, docs/vegetation-api.js
    // "Sward"), the data its swardData(i) reads, the blades' mean height,
    // the tallest one and how far one reaches from its root.
    function setSward(node, sward) {
        const data = sward.data instanceof Float32Array ? sward.data
            : new Float32Array(Array.isArray(sward.data) ? sward.data : 0);
        const height = typeof sward.height === 'number' ? sward.height : 0.4;
        __bro_native.scene.SceneNode_setGrassSward(node, typeof sward.source === 'string' ? sward.source : '', data,
            height, typeof sward.tallest === 'number' ? sward.tallest : height,
            typeof sward.reach === 'number' ? sward.reach : 0);
    }

    // The meshes shaded as the grass's ground (turf: true is the ground
    // node itself).
    function applyTurf(node, opts) {
        let t = opts.turf;
        if (t === undefined || t === null || t === false) return;
        if (t === true) t = opts.ground;
        const list = Array.isArray(t) ? t : [t];
        for (const m of list) if (m instanceof SceneNode) __bro_native.scene.SceneNode_setTurfGrass(m, node, 1);
    }

    // The look and the material. `density` in the options is the field's
    // (where grass grows); the node param of that name is `bladeDensity`.
    function applyOpts(node, opts) {
        const o = Object.assign({}, opts);
        delete o.density;
        if (typeof opts.bladeDensity === 'number') o.density = opts.bladeDensity;
        __bro_native.scene.SceneNode_setGrassParams(node, paramArray(o));
        if (opts.sward) setSward(node, opts.sward);
        const keys = ['name', 'color', 'roughness', 'castsShadow', 'receivesShadow', 'visible'];
        for (const k of keys) if (opts[k] !== undefined) node[k] = opts[k];
    }

    fn(SceneGraph.prototype, 'createGrass', function createGrass(opts) {
        const o = opts || {};
        const node = canon(__bro_native.scene.SceneGraph_createGrass(this));
        if (!node) return node;
        applyOpts(node, o);
        bake(node, o);
        applyTurf(node, o);
        return node;
    });

    const P = SceneNode.prototype;
    fn(P, 'setGround', function setGround(opts) {
        if (isGrass(this)) {
            bake(this, opts || {});
            applyTurf(this, opts || {});
        }
        return this;
    });
    // Shade mesh node `mesh` as this grass's ground (on false: as itself).
    fn(P, 'shadeGround', function shadeGround(mesh, on) {
        if (isGrass(this) && mesh instanceof SceneNode)
            __bro_native.scene.SceneNode_setTurfGrass(mesh, this, on === false ? 0 : 1);
        return this;
    });
    fn(P, 'setGrass', function setGrass(opts) {
        if (isGrass(this) && opts) applyOpts(this, opts);
        return this;
    });
    // Swap the herb module (and its data and range) without rebaking the
    // floor.
    fn(P, 'setHerbs', function setHerbsMember(herbs) {
        if (isGrass(this) && herbs) setHerbs(this, herbs);
        return this;
    });
    // Swap the sward module (and its data) without rebaking the field.
    fn(P, 'setSward', function setSwardMember(sward) {
        if (isGrass(this) && sward) setSward(this, sward);
        return this;
    });
    fn(P, 'grassParams', function grassParams() {
        const a = __bro_native.scene.SceneNode_grassParams(this);
        if (a.length !== PARAM_COUNT) return undefined;
        return { width: a[0], bladeDensity: a[1], range: a[2] };
    });
    fn(P, 'addMover', function addMover(target, mopts) {
        if (!isGrass(this) || !(target instanceof SceneNode)) return this;
        const m = mopts || {};
        __bro_native.scene.SceneNode_setGrassMover(this, target, m.radius > 0 ? +m.radius : 0.6,
            typeof m.strength === 'number' ? m.strength : 1, typeof m.trample === 'number' ? m.trample : 0,
            typeof m.height === 'number' ? m.height : 1.8);
        return this;
    });
    fn(P, 'removeMover', function removeMover(target) {
        if (isGrass(this) && target instanceof SceneNode) __bro_native.scene.SceneNode_setGrassMover(this, target, 0, 0, 0, 0);
        return this;
    });
    fn(P, 'push', function push(x, z, popts) {
        if (!isGrass(this)) return this;
        const p = popts || {};
        __bro_native.scene.SceneNode_grassPush(this, +x, +z, p.radius > 0 ? +p.radius : 0.6, +(p.vx || 0), +(p.vz || 0),
            typeof p.strength === 'number' ? p.strength : 1, typeof p.trample === 'number' ? p.trample : 0);
        return this;
    });
    fn(P, 'grassStats', function grassStats() {
        const s = __bro_native.scene.SceneNode_grassStats(this);
        if (s.length < 5) return undefined;
        // The blade rings' count and one count each, then movers and the
        // field's size, then one count a herb ring.
        const n = s[0] + 1;
        return {
            instances: Array.from(s.slice(1, n)), movers: s[n], fieldCols: s[n + 1], fieldRows: s[n + 2],
            herbInstances: Array.from(s.slice(n + 3)),
        };
    });
    // The ground height the grass stands on at (x, z): the field, bilinear.
    fn(P, 'grassHeightAt', function grassHeightAt(x, z) {
        const f = fields.get(this);
        if (!f) return undefined;
        const u = Math.min(Math.max((x - f.x0) / f.cell, 0), f.cols - 1);
        const v = Math.min(Math.max((z - f.z0) / f.cell, 0), f.rows - 1);
        const i = Math.min(Math.floor(u), f.cols - 2), j = Math.min(Math.floor(v), f.rows - 2);
        const fu = u - i, fv = v - j;
        const h = (a, b) => f.data[((j + b) * f.cols + i + a) * 4];
        return (h(0, 0) * (1 - fu) + h(1, 0) * fu) * (1 - fv) + (h(0, 1) * (1 - fu) + h(1, 1) * fu) * fv;
    });
    // A mesh node's top surface on a grid (NaN where it has none).
    fn(P, 'sampleHeights', function sampleHeights(x0, z0, cell, cols, rows) {
        return __bro_native.scene.SceneNode_sampleHeights(this, +x0, +z0, +cell, cols | 0, rows | 0);
    });
})();
