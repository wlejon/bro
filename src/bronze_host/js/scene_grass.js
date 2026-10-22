// scene_grass.js — SceneGraph.createGrass and the grass node's members over
// native_scene_grass.cpp.
//
// The node (scene/nodes/grass_node.h) draws blades over the land it stands
// on (scene.createLand): where they grow, how thickly, how dry and how tall,
// the canopy over them and the herbs among them are the land's surface
// module's (docs/land-api.js), read from its tiles on the GPU, so the grass
// reaches as far as the land does. What the blades and herbs are is the
// node's sward and herb modules.
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

    // Stand the grass on land node `land`.
    function setGround(node, land, who) {
        if (!(land instanceof SceneNode) || !__bro_native.scene.SceneNode_setGrassGround(node, land))
            throw new TypeError(`${who}: the ground must be a land node (scene.createLand)`);
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

    // The meshes shaded as the grass's ground (turf: true is the land
    // itself).
    function applyTurf(node, opts) {
        let t = opts.turf;
        if (t === undefined || t === null || t === false) return;
        if (t === true) t = opts.ground;
        const list = Array.isArray(t) ? t : [t];
        for (const m of list) if (m instanceof SceneNode) __bro_native.scene.SceneNode_setTurfGrass(m, node, 1);
    }

    // The look, the modules and the material. The node param `density` is
    // `bladeDensity` in the options.
    function applyOpts(node, opts) {
        const o = Object.assign({}, opts);
        delete o.density;
        if (typeof opts.bladeDensity === 'number') o.density = opts.bladeDensity;
        __bro_native.scene.SceneNode_setGrassParams(node, paramArray(o));
        if (opts.sward) setSward(node, opts.sward);
        if (opts.herbs) setHerbs(node, opts.herbs);
        const keys = ['name', 'color', 'roughness', 'castsShadow', 'receivesShadow', 'visible'];
        for (const k of keys) if (opts[k] !== undefined) node[k] = opts[k];
    }

    fn(SceneGraph.prototype, 'createGrass', function createGrass(opts) {
        const o = opts || {};
        if (!(o.ground instanceof SceneNode)) throw new TypeError('createGrass: opts.ground must be a land node (scene.createLand)');
        const node = canon(__bro_native.scene.SceneGraph_createGrass(this));
        if (!node) return node;
        setGround(node, o.ground, 'createGrass');
        applyOpts(node, o);
        applyTurf(node, o);
        return node;
    });

    const P = SceneNode.prototype;
    // Stand the grass on another land.
    fn(P, 'setGround', function setGroundMember(land) {
        if (isGrass(this)) setGround(this, land, 'setGround');
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
    // Swap the herb module (and its data and range).
    fn(P, 'setHerbs', function setHerbsMember(herbs) {
        if (isGrass(this) && herbs) setHerbs(this, herbs);
        return this;
    });
    // Swap the sward module (and its data).
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
        if (s.length < 3) return undefined;
        // The blade rings' count and one count each, then movers, then one
        // count a herb ring.
        const n = s[0] + 1;
        return { instances: Array.from(s.slice(1, n)), movers: s[n], herbInstances: Array.from(s.slice(n + 1)) };
    });
    // A mesh node's top surface on a grid (NaN where it has none); a land's
    // heights.
    fn(P, 'sampleHeights', function sampleHeights(x0, z0, cell, cols, rows) {
        return __bro_native.scene.SceneNode_sampleHeights(this, +x0, +z0, +cell, cols | 0, rows | 0);
    });
})();
