// scene.js — the public shape of bro.scene.SceneNode, bro.scene.SceneGraph.
(function () {
    'use strict';

    const accessor = (obj, name, get, set) =>
        Object.defineProperty(obj, name, { get, set, enumerable: true, configurable: true });
    const fn = (obj, name, value) =>
        Object.defineProperty(obj, name, { value, writable: true, enumerable: true, configurable: true });
    const EMPTY_F32 = new Float32Array(0);
    const EMPTY_F64 = new Float64Array(0);
    const EMPTY_I32 = new Int32Array(0);
    const mount = (root, name) => root[name] !== undefined ? root[name] : (root[name] = {});
    const toF32 = (v) => v instanceof Float32Array ? v : (typeof v === 'number' ? new Float32Array([v]) : (Array.isArray(v) ? Float32Array.from(v) : EMPTY_F32));
    const toF64 = (v) => v instanceof Float64Array ? v : (typeof v === 'number' ? new Float64Array([v]) : (Array.isArray(v) ? Float64Array.from(v) : EMPTY_F64));
    const toI32 = (v) => v instanceof Int32Array ? v : (typeof v === 'number' ? new Int32Array([v]) : (Array.isArray(v) ? Int32Array.from(v) : EMPTY_I32));
    const isMeshLike = (node) => {
        const t = __bro_native.scene.SceneNode_type_get(node);
        return t === 'mesh' || t === 'skinnedMesh' || t === 'instancedMesh';
    };
    const applyNodeOpts = (node, opts) => {
        if (!node || !opts) return;
        const keys = ['name', 'x', 'y', 'z', 'position', 'rotation', 'scale', 'color',
                      'metallic', 'roughness', 'emissive', 'cullMargin', 'castsShadow', 'receivesShadow',
                      'visible', 'updateMode', 'resolution', 'boxProjection', 'intensity',
                      'interior', 'priority', 'worldAnchor', 'rx', 'ry', 'rz',
                      'rotationX', 'rotationY', 'rotationZ'];
        for (let i = 0; i < keys.length; i++) {
            const k = keys[i];
            if (opts[k] !== undefined) {
                if (k === 'color' && typeof opts.color === 'object' && !Array.isArray(opts.color)) continue;
                node[k] = opts[k];
            }
        }
        if (opts.fill !== undefined && opts.color === undefined) node.color = opts.fill;
    };


    // ---- bro.scene.SceneNode -------------------------------------------------
    function SceneNode() {
        throw new TypeError("bro.scene.SceneNode is not constructible: instances come from the natives that return one");
    }
    {
        const proto = __bro_native.scene.SceneNodeProto;
        if (proto === undefined) throw new Error("bro.scene.SceneNode: native class prototype not published (registerNatives_scene did not run)");
        Object.setPrototypeOf(proto, SceneNode.prototype);
    }
    fn(mount(bro, "scene"), "SceneNode", SceneNode);
    globalThis.SceneNode = SceneNode;
    accessor(SceneNode.prototype, "id", function () { return __bro_native.scene.SceneNode_id_get(this); }, undefined);
    accessor(SceneNode.prototype, "name", function () { return __bro_native.scene.SceneNode_name_get(this); }, function (v) { __bro_native.scene.SceneNode_name_set(this, v); });
    accessor(SceneNode.prototype, "visible", function () { return __bro_native.scene.SceneNode_visible_get(this); }, function (v) { __bro_native.scene.SceneNode_visible_set(this, v); });

    accessor(SceneNode.prototype, "position",
        function () {
            if (this.id === 0) return undefined;
            return Array.from(__bro_native.scene.SceneNode_position_get(this));
        },
        function (v) {
            if (this.id === 0) return;
            __bro_native.scene.SceneNode_position_set(this, toF64(v));
        });
    accessor(SceneNode.prototype, "rotation",
        function () {
            if (this.id === 0) return 0;
            return __bro_native.scene.SceneNode_rotationZ_get(this);
        },
        function (v) {
            if (this.id === 0) return;
            if (typeof v === 'number') {
                __bro_native.scene.SceneNode_rotationZ_set(this, +v);
            } else if (Array.isArray(v) || (v && typeof v.length === 'number')) {
                __bro_native.scene.SceneNode_rotation_set(this, toF64(v));
            }
        });
    accessor(SceneNode.prototype, "scale",
        function () {
            if (this.id === 0) return undefined;
            return Array.from(__bro_native.scene.SceneNode_scale_get(this));
        },
        function (v) {
            if (this.id === 0) return;
            if (typeof v === 'number') {
                __bro_native.scene.SceneNode_scale_set(this, toF64([v, v, v]));
            } else if (Array.isArray(v) || (v && typeof v.length === 'number')) {
                const sx = v[0] !== undefined ? +v[0] : 1;
                const sy = v[1] !== undefined ? +v[1] : 1;
                const sz = v[2] !== undefined ? +v[2] : 1;
                __bro_native.scene.SceneNode_scale_set(this, toF64([sx, sy, sz]));
            } else {
                __bro_native.scene.SceneNode_scale_set(this, toF64(v));
            }
        });
    accessor(SceneNode.prototype, "worldPosition", function () { return Array.from(__bro_native.scene.SceneNode_worldPosition_get(this)); }, undefined);
    accessor(SceneNode.prototype, "worldMatrix", function () { return Array.from(__bro_native.scene.SceneNode_worldMatrix_get(this)); }, undefined);
    accessor(SceneNode.prototype, "parent", function () { return this.id === 0 ? null : __bro_native.scene.SceneNode_parent_get(this); }, undefined);

    accessor(SceneNode.prototype, "children",
        function () {
            if (this.id === 0) return [];
            const n = __bro_native.scene.SceneNode_children_get(this);
            const out = new Array(n);
            for (let i = 0; i < n; i++) out[i] = __bro_native.scene.SceneNode_children_get_at(i);
            return out;
        },
        undefined);
    fn(SceneNode.prototype, "add", function add(child) { if (child === undefined) throw new TypeError("child is required"); __bro_native.scene.SceneNode_add(this, child); return this; });
    fn(SceneNode.prototype, "remove", function remove(child) { if (child === undefined) throw new TypeError("child is required"); __bro_native.scene.SceneNode_remove(this, child); });
    fn(SceneNode.prototype, "addChild", function addChild(child) { if (child === undefined) throw new TypeError("child is required"); __bro_native.scene.SceneNode_addChild(this, child); return this; });
    fn(SceneNode.prototype, "removeChild", function removeChild(child) { if (child === undefined) throw new TypeError("child is required"); __bro_native.scene.SceneNode_removeChild(this, child); });
    fn(SceneNode.prototype, "destroy", function destroy() { __bro_native.scene.SceneNode_destroy(this); });
    fn(SceneNode.prototype, "setPosition", function setPosition(x, y, z) {
        if (x === undefined || y === undefined || z === undefined) throw new TypeError("x, y, z are required");
        __bro_native.scene.SceneNode_setPosition(this, x, y, z);
        return this;
    });
    fn(SceneNode.prototype, "setRotation", function setRotation(x, y, z, w) {
        if (x === undefined || y === undefined || z === undefined) throw new TypeError("x, y, z are required");
        __bro_native.scene.SceneNode_setRotation(this, x, y, z, w !== undefined, w === undefined ? 0 : w);
        return this;
    });
    fn(SceneNode.prototype, "setScale", function setScale(x, y, z) {
        if (x === undefined) throw new TypeError("x is required");
        __bro_native.scene.SceneNode_setScale(this, x, y !== undefined, y === undefined ? 0 : y, z !== undefined, z === undefined ? 0 : z);
        return this;
    });
    fn(SceneNode.prototype, "lookAt", function lookAt(target, up) {
        if (target === undefined) throw new TypeError("target is required");
        let t = target, u = up;
        if (typeof target === 'number' && arguments.length >= 3) { t = [arguments[0], arguments[1], arguments[2]]; u = arguments[3]; }
        __bro_native.scene.SceneNode_lookAt(this, toF64(t), u === undefined ? EMPTY_F64 : toF64(u));
        return this;
    });
    fn(SceneNode.prototype, "setSkeleton", function setSkeleton(skeleton) { if (skeleton === undefined) throw new TypeError("skeleton is required"); __bro_native.scene.SceneNode_setSkeleton(this, skeleton); return this; });
    fn(SceneNode.prototype, "addClip", function addClip(name, anim) { if (name === undefined || anim === undefined) throw new TypeError("name and anim are required"); __bro_native.scene.SceneNode_addClip(this, name, anim); return this; });
    fn(SceneNode.prototype, "getBoneWorldMatrix", function getBoneWorldMatrix(bone) { if (bone === undefined) throw new TypeError("bone is required"); return __bro_native.scene.SceneNode_getBoneWorldMatrix(this, bone); });
    fn(SceneNode.prototype, "addBlendSpace1D", function addBlendSpace1D(name, clips) { if (name === undefined || clips === undefined) throw new TypeError("name and clips are required"); __bro_native.scene.SceneNode_addBlendSpace1D(this, name, JSON.stringify(clips)); return this; });
    fn(SceneNode.prototype, "addBlendSpace2D", function addBlendSpace2D(name, clips) { if (name === undefined || clips === undefined) throw new TypeError("name and clips are required"); __bro_native.scene.SceneNode_addBlendSpace2D(this, name, JSON.stringify(clips)); return this; });
    fn(SceneNode.prototype, "setBlendPos", function setBlendPos(name, x, y) {
        if (name === undefined) throw new TypeError("bro.scene.SceneNode.prototype.setBlendPos: name is required");
        if (x === undefined) throw new TypeError("bro.scene.SceneNode.prototype.setBlendPos: x is required");
        let px = x, py = y;
        if (Array.isArray(x)) { px = x[0] !== undefined ? x[0] : 0; py = x[1] !== undefined ? x[1] : 0; }
        __bro_native.scene.SceneNode_setBlendPos(this, name, px, py !== undefined, py === undefined ? 0 : py);
        return this;
    });
    fn(SceneNode.prototype, "blendState", function blendState() {
        const s = __bro_native.scene.SceneNode_blendState(this);
        return s ? JSON.parse(s) : {};
    });
    fn(SceneNode.prototype, "playLayer", function playLayer(layer, clipName, opts) {
        if (layer === undefined) throw new TypeError("bro.scene.SceneNode.prototype.playLayer: layer is required");
        if (clipName === undefined) throw new TypeError("bro.scene.SceneNode.prototype.playLayer: clipName is required");
        let jsonOpts = "";
        if (typeof opts === 'object' && opts !== null) {
            jsonOpts = JSON.stringify(opts, (k, v) => {
                if (v instanceof Float32Array || v instanceof Float64Array || v instanceof Uint32Array || v instanceof Uint8Array || v instanceof Int32Array) {
                    return Array.from(v);
                }
                return v;
            });
        } else if (typeof opts === 'number') {
            jsonOpts = JSON.stringify({ weight: opts });
        }
        __bro_native.scene.SceneNode_playLayer(this, layer, clipName, jsonOpts);
        return this;
    });
    fn(SceneNode.prototype, "stopLayer", function stopLayer(layer, opts) {
        if (layer === undefined) throw new TypeError("bro.scene.SceneNode.prototype.stopLayer: layer is required");
        let ft = 0, hasFt = false;
        if (typeof opts === 'number') { ft = opts; hasFt = true; }
        else if (typeof opts === 'object' && opts !== null && opts.fadeTime !== undefined) { ft = opts.fadeTime; hasFt = true; }
        __bro_native.scene.SceneNode_stopLayer(this, layer, hasFt, ft);
        return this;
    });
    fn(SceneNode.prototype, "setLayerWeight", function setLayerWeight(layer, weight) {
        if (layer === undefined) throw new TypeError("bro.scene.SceneNode.prototype.setLayerWeight: layer is required");
        if (weight === undefined) throw new TypeError("bro.scene.SceneNode.prototype.setLayerWeight: weight is required");
        __bro_native.scene.SceneNode_setLayerWeight(this, layer, weight);
        return this;
    });
    fn(SceneNode.prototype, "addStateMachine", function addStateMachine(def) {
        if (def === undefined) throw new TypeError("bro.scene.SceneNode.prototype.addStateMachine: def is required");
        __bro_native.scene.SceneNode_addStateMachine(this, typeof def === 'string' ? def : JSON.stringify(def));
        return this;
    });
    fn(SceneNode.prototype, "travel", function travel(targetState) {
        if (targetState === undefined) throw new TypeError("bro.scene.SceneNode.prototype.travel: targetState is required");
        __bro_native.scene.SceneNode_travel(this, targetState);
        return this;
    });
    fn(SceneNode.prototype, "setRootMotion", function setRootMotion(opts) {
        if (opts === undefined) throw new TypeError("bro.scene.SceneNode.prototype.setRootMotion: opts is required");
        const o = typeof opts === 'boolean' ? { enabled: opts } : (opts || {});
        __bro_native.scene.SceneNode_setRootMotion(this, JSON.stringify(o));
        return this;
    });
    fn(SceneNode.prototype, "consumeRootMotion", function consumeRootMotion() {
        const s = __bro_native.scene.SceneNode_consumeRootMotion(this);
        return s ? JSON.parse(s) : { translation: [0, 0, 0], yaw: 0 };
    });
    fn(SceneNode.prototype, "play", function play(clipName, opts) {
        if (this.id === 0) return this;
        let jsonOpts = "";
        if (typeof opts === 'object' && opts !== null) {
            jsonOpts = JSON.stringify(opts, (k, v) => {
                if (v instanceof Float32Array || v instanceof Float64Array || v instanceof Uint32Array || v instanceof Uint8Array || v instanceof Int32Array) {
                    return Array.from(v);
                }
                return v;
            });
        }
        __bro_native.scene.SceneNode_play(this, clipName !== undefined ? String(clipName) : "", jsonOpts);
        return this;
    });
    fn(SceneNode.prototype, "stop", function stop(opts) {
        let jsonOpts = "";
        if (typeof opts === 'number') jsonOpts = JSON.stringify({ fadeTime: opts });
        else if (typeof opts === 'object' && opts !== null) jsonOpts = JSON.stringify(opts);
        __bro_native.scene.SceneNode_stop(this, jsonOpts);
        return this;
    });
    fn(SceneNode.prototype, "pause", function pause() { __bro_native.scene.SceneNode_pause(this); return this; });
    fn(SceneNode.prototype, "resume", function resume() { __bro_native.scene.SceneNode_resume(this); return this; });
    fn(SceneNode.prototype, "setSkinningMatrices", function setSkinningMatrices(mats) {
        if (mats === undefined) throw new TypeError("bro.scene.SceneNode.prototype.setSkinningMatrices: mats is required");
        return __bro_native.scene.SceneNode_setSkinningMatrices(this, mats);
    });
    fn(SceneNode.prototype, "setInstanceTransform", function setInstanceTransform(index, matrix) {
        if (index === undefined) throw new TypeError("bro.scene.SceneNode.prototype.setInstanceTransform: index is required");
        if (matrix === undefined) throw new TypeError("bro.scene.SceneNode.prototype.setInstanceTransform: matrix is required");
        __bro_native.scene.SceneNode_setInstanceTransform(this, index, toF64(matrix));
    });
    fn(SceneNode.prototype, "setInstanceColor", function setInstanceColor(index, color) {
        if (index === undefined) throw new TypeError("bro.scene.SceneNode.prototype.setInstanceColor: index is required");
        if (color === undefined) throw new TypeError("bro.scene.SceneNode.prototype.setInstanceColor: color is required");
        __bro_native.scene.SceneNode_setInstanceColor(this, index, toF64(color));
    });
    fn(SceneNode.prototype, "setInstanceCount", function setInstanceCount(count) {
        if (count === undefined) throw new TypeError("bro.scene.SceneNode.prototype.setInstanceCount: count is required");
        __bro_native.scene.SceneNode_setInstanceCount(this, count);
    });
    fn(SceneNode.prototype, "setHtml", function setHtml(html) {
        if (html === undefined) throw new TypeError("bro.scene.SceneNode.prototype.setHtml: html is required");
        return __bro_native.scene.SceneNode_setHtml(this, html);
    });
    fn(SceneNode.prototype, "markHtmlDirty", function markHtmlDirty() { __bro_native.scene.SceneNode_markHtmlDirty(this); });
    fn(SceneNode.prototype, "burst", function burst(count) {
        if (count === undefined) throw new TypeError("bro.scene.SceneNode.prototype.burst: count is required");
        __bro_native.scene.SceneNode_burst(this, count);
    });
    fn(SceneNode.prototype, "clear", function clear() { __bro_native.scene.SceneNode_clear(this); });
    fn(SceneNode.prototype, "probeCapture", function probeCapture() { __bro_native.scene.SceneNode_probeCapture(this); });
    fn(SceneNode.prototype, "savePly", function savePly(path) {
        if (path === undefined) throw new TypeError("bro.scene.SceneNode.prototype.savePly: path is required");
        if (this.type !== 'gaussianSplat') throw new TypeError("savePly: node is not a GaussianSplat");
        if (this.splatCount === 0) throw new TypeError("savePly: splat cloud is empty");
        const ok = __bro_native.scene.SceneNode_savePly(this, path);
        if (!ok) throw new Error("savePly: failed to write file " + path);
        return true;
    });
    fn(SceneNode.prototype, "setCloud", function setCloud(c) {
        if (!c) return this;
        __bro_native.scene.SceneNode_setCloud(this,
            c.positions ? toF32(c.positions) : EMPTY_F32,
            c.scales ? toF32(c.scales) : EMPTY_F32,
            c.rotations ? toF32(c.rotations) : EMPTY_F32,
            c.opacities ? toF32(c.opacities) : EMPTY_F32,
            c.sh ? toF32(c.sh) : EMPTY_F32,
            c.shDegree !== undefined ? (c.shDegree | 0) : 0);
        return this;
    });

    function parseColor(c) {
        if (typeof c === 'string') {
            if (c.startsWith('#')) {
                if (c.length === 7) return [parseInt(c.slice(1, 3), 16) / 255, parseInt(c.slice(3, 5), 16) / 255, parseInt(c.slice(5, 7), 16) / 255];
                if (c.length === 4) return [parseInt(c[1] + c[1], 16) / 255, parseInt(c[2] + c[2], 16) / 255, parseInt(c[3] + c[3], 16) / 255];
            }
            const map = { red: [1, 0, 0], green: [0, 1, 0], blue: [0, 0, 1], white: [1, 1, 1], black: [0, 0, 0] };
            if (map[c]) return map[c];
        }
        return c;
    }

    accessor(SceneNode.prototype, "x", function () { return this.id === 0 ? 0 : __bro_native.scene.SceneNode_x_get(this); }, function (v) { if (this.id !== 0) __bro_native.scene.SceneNode_x_set(this, +v); });
    accessor(SceneNode.prototype, "y", function () { return this.id === 0 ? 0 : __bro_native.scene.SceneNode_y_get(this); }, function (v) { if (this.id !== 0) __bro_native.scene.SceneNode_y_set(this, +v); });
    accessor(SceneNode.prototype, "z", function () { return this.id === 0 ? 0 : __bro_native.scene.SceneNode_z_get(this); }, function (v) { if (this.id !== 0) __bro_native.scene.SceneNode_z_set(this, +v); });
    accessor(SceneNode.prototype, "rotationX", function () { return this.id === 0 ? 0 : __bro_native.scene.SceneNode_rotationX_get(this); }, function (v) { if (this.id !== 0) __bro_native.scene.SceneNode_rotationX_set(this, +v); });
    accessor(SceneNode.prototype, "rotationY", function () { return this.id === 0 ? 0 : __bro_native.scene.SceneNode_rotationY_get(this); }, function (v) { if (this.id !== 0) __bro_native.scene.SceneNode_rotationY_set(this, +v); });
    accessor(SceneNode.prototype, "rotationZ", function () { return this.id === 0 ? 0 : __bro_native.scene.SceneNode_rotationZ_get(this); }, function (v) { if (this.id !== 0) __bro_native.scene.SceneNode_rotationZ_set(this, +v); });
    accessor(SceneNode.prototype, "rx", function () { return this.rotationX; }, function (v) { this.rotationX = v; });
    accessor(SceneNode.prototype, "ry", function () { return this.rotationY; }, function (v) { this.rotationY = v; });
    accessor(SceneNode.prototype, "rz", function () { return this.rotationZ; }, function (v) { this.rotationZ = v; });
    accessor(SceneNode.prototype, "scaleX", function () { return this.id === 0 ? 1 : __bro_native.scene.SceneNode_scaleX_get(this); }, function (v) { if (this.id !== 0) __bro_native.scene.SceneNode_scaleX_set(this, +v); });
    accessor(SceneNode.prototype, "scaleY", function () { return this.id === 0 ? 1 : __bro_native.scene.SceneNode_scaleY_get(this); }, function (v) { if (this.id !== 0) __bro_native.scene.SceneNode_scaleY_set(this, +v); });
    accessor(SceneNode.prototype, "scaleZ", function () { return this.id === 0 ? 1 : __bro_native.scene.SceneNode_scaleZ_get(this); }, function (v) { if (this.id !== 0) __bro_native.scene.SceneNode_scaleZ_set(this, +v); });
    accessor(SceneNode.prototype, "quaternion", function () { return this.id === 0 ? undefined : Array.from(__bro_native.scene.SceneNode_quaternion_get(this)); }, function (v) { if (this.id !== 0) __bro_native.scene.SceneNode_quaternion_set(this, toF64(v)); });
    fn(SceneNode.prototype, "localToWorld", function localToWorld(x, y, z) {
        if (this.id === 0) return { x: 0, y: 0, z: 0 };
        const m = __bro_native.scene.SceneNode_worldMatrix_get(this);
        if (!m || m.length < 16) return { x: 0, y: 0, z: 0 };
        const px = +x;
        const py = +y;
        const pz = z !== undefined ? +z : 0;
        return {
            x: m[0] * px + m[4] * py + m[8] * pz + m[12],
            y: m[1] * px + m[5] * py + m[9] * pz + m[13],
            z: m[2] * px + m[6] * py + m[10] * pz + m[14],
        };
    });
    fn(SceneNode.prototype, "worldToLocal", function worldToLocal(x, y, z) {
        if (this.id === 0) return { x: 0, y: 0, z: 0 };
        const m = __bro_native.scene.SceneNode_worldMatrix_get(this);
        if (!m || m.length < 16) return { x: 0, y: 0, z: 0 };
        const px = +x;
        const py = +y;
        const pz = z !== undefined ? +z : 0;
        const m00 = m[0], m01 = m[4], m02 = m[8],  m03 = m[12];
        const m10 = m[1], m11 = m[5], m12 = m[9],  m13 = m[13];
        const m20 = m[2], m21 = m[6], m22 = m[10], m23 = m[14];
        const dx = px - m03;
        const dy = py - m13;
        const dz = pz - m23;
        const c00 = m11 * m22 - m12 * m21;
        const c01 = m12 * m20 - m10 * m22;
        const c02 = m10 * m21 - m11 * m20;
        const det = m00 * c00 + m01 * c01 + m02 * c02;
        if (Math.abs(det) < 1e-12) return { x: 0, y: 0, z: 0 };
        const invDet = 1.0 / det;
        const c10 = m02 * m21 - m01 * m22;
        const c11 = m00 * m22 - m02 * m20;
        const c12 = m01 * m20 - m00 * m21;
        const c20 = m01 * m12 - m02 * m11;
        const c21 = m02 * m10 - m00 * m12;
        const c22 = m00 * m11 - m01 * m10;
        return {
            x: (c00 * dx + c10 * dy + c20 * dz) * invDet,
            y: (c01 * dx + c11 * dy + c21 * dz) * invDet,
            z: (c02 * dx + c12 * dy + c22 * dz) * invDet,
        };
    });
    accessor(SceneNode.prototype, "childCount", function () { return __bro_native.scene.SceneNode_childCount_get(this); }, undefined);
    accessor(SceneNode.prototype, "type", function () {
        if (this.id === 0) return undefined;
        const t = __bro_native.scene.SceneNode_type_get(this);
        return (t === 'none' || !t) ? undefined : t;
    }, undefined);
    accessor(SceneNode.prototype, "splatCount", function () {
        return this.id === 0 ? 0 : __bro_native.scene.SceneNode_splatCount_get(this);
    }, undefined);
    accessor(SceneNode.prototype, "worldAnchor",
        function () {
            if (this.id === 0) return null;
            return Array.from(__bro_native.scene.SceneNode_worldAnchor_get(this));
        },
        function (v) {
            if (this.id !== 0) {
                if (!v) __bro_native.scene.SceneNode_worldAnchor_set(this, EMPTY_F64);
                else __bro_native.scene.SceneNode_worldAnchor_set(this, toF64(v));
            }
        });
    accessor(SceneNode.prototype, "kind", function () { return __bro_native.scene.SceneNode_kind_get(this); }, undefined);
    accessor(SceneNode.prototype, "castsShadow",
        function () { return __bro_native.scene.SceneNode_castsShadow_get(this); },
        function (v) { __bro_native.scene.SceneNode_castsShadow_set(this, !!v); });
    accessor(SceneNode.prototype, "receivesShadow",
        function () { return __bro_native.scene.SceneNode_receivesShadow_get(this); },
        function (v) { __bro_native.scene.SceneNode_receivesShadow_set(this, !!v); });
    accessor(SceneNode.prototype, "metallic", function () { return isMeshLike(this) ? __bro_native.scene.SceneNode_metallic_get(this) : undefined; }, function (v) { if (isMeshLike(this)) __bro_native.scene.SceneNode_metallic_set(this, +v); });
    accessor(SceneNode.prototype, "roughness", function () { return isMeshLike(this) ? __bro_native.scene.SceneNode_roughness_get(this) : undefined; }, function (v) { if (isMeshLike(this)) __bro_native.scene.SceneNode_roughness_set(this, +v); });
    accessor(SceneNode.prototype, "emissive", function () { return isMeshLike(this) ? __bro_native.scene.SceneNode_emissive_get(this) : undefined; }, function (v) { if (isMeshLike(this)) __bro_native.scene.SceneNode_emissive_set(this, +v); });
    accessor(SceneNode.prototype, "cullMargin", function () { return isMeshLike(this) ? __bro_native.scene.SceneNode_cullMargin_get(this) : undefined; }, function (v) { if (isMeshLike(this)) __bro_native.scene.SceneNode_cullMargin_set(this, +v); });
    accessor(SceneNode.prototype, "boneCount", function () { return __bro_native.scene.SceneNode_boneCount_get(this); }, undefined);
    accessor(SceneNode.prototype, "skinReady", function () { return __bro_native.scene.SceneNode_skinReady_get(this); }, undefined);
    accessor(SceneNode.prototype, "isPlaying", function () {
        return this.type === 'particles3d' ? __bro_native.scene.SceneNode_particlePlaying_get(this) : __bro_native.scene.SceneNode_isPlaying_get(this);
    }, undefined);
    accessor(SceneNode.prototype, "currentAnimation", function () { const c = __bro_native.scene.SceneNode_currentAnimation_get(this); return c ? c : null; }, undefined);
    accessor(SceneNode.prototype, "animationDuration", function () { return __bro_native.scene.SceneNode_animationDuration_get(this); }, undefined);
    accessor(SceneNode.prototype, "animationTime", function () { return __bro_native.scene.SceneNode_animationTime_get(this); }, function (v) { __bro_native.scene.SceneNode_animationTime_set(this, +v); });
    accessor(SceneNode.prototype, "animationSpeed", function () { return __bro_native.scene.SceneNode_animationSpeed_get(this); }, function (v) { __bro_native.scene.SceneNode_animationSpeed_set(this, +v); });
    accessor(SceneNode.prototype, "state", function () { const s = __bro_native.scene.SceneNode_state_get(this); return s ? s : null; }, undefined);
    accessor(SceneNode.prototype, "onAnimationFinished", function () { return this._onAnimationFinished; }, function (v) { this._onAnimationFinished = v; __bro_native.scene.SceneNode_onAnimationFinished_set(this, v); });
    accessor(SceneNode.prototype, "onStateChanged", function () { return this._onStateChanged; }, function (v) { this._onStateChanged = v; __bro_native.scene.SceneNode_onStateChanged_set(this, v); });
    accessor(SceneNode.prototype, "direction", function () { return Array.from(__bro_native.scene.SceneNode_direction_get(this)); }, function (v) { __bro_native.scene.SceneNode_direction_set(this, toF64(v)); });
    accessor(SceneNode.prototype, "color", function () { return Array.from(__bro_native.scene.SceneNode_color_get(this)); }, function (v) { __bro_native.scene.SceneNode_color_set(this, toF64(parseColor(v))); });
    accessor(SceneNode.prototype, "intensity", function () { return __bro_native.scene.SceneNode_intensity_get(this); }, function (v) { __bro_native.scene.SceneNode_intensity_set(this, +v); });
    accessor(SceneNode.prototype, "range", function () { return __bro_native.scene.SceneNode_range_get(this); }, function (v) { __bro_native.scene.SceneNode_range_set(this, +v); });
    accessor(SceneNode.prototype, "innerAngle", function () { return __bro_native.scene.SceneNode_innerAngle_get(this); }, function (v) { __bro_native.scene.SceneNode_innerAngle_set(this, +v); });
    accessor(SceneNode.prototype, "outerAngle", function () { return __bro_native.scene.SceneNode_outerAngle_get(this); }, function (v) { __bro_native.scene.SceneNode_outerAngle_set(this, +v); });
    accessor(SceneNode.prototype, "particleCount", function () { return __bro_native.scene.SceneNode_particleCount_get(this); }, undefined);
    accessor(SceneNode.prototype, "liveCount", function () { return __bro_native.scene.SceneNode_particleCount_get(this); }, undefined);
    accessor(SceneNode.prototype, "rate", function () { return __bro_native.scene.SceneNode_particleRate_get(this); }, function (v) { __bro_native.scene.SceneNode_particleRate_set(this, +v); });
    accessor(SceneNode.prototype, "softness", function () { return __bro_native.scene.SceneNode_softness_get(this); }, function (v) { __bro_native.scene.SceneNode_softness_set(this, +v); });
    fn(SceneNode.prototype, "burst", function (n) { __bro_native.scene.SceneNode_burst(this, n | 0); return this; });
    fn(SceneNode.prototype, "clear", function () { __bro_native.scene.SceneNode_clear(this); return this; });
    accessor(SceneNode.prototype, "onFinished", function () { return this._onFinished; }, function (v) { this._onFinished = v; __bro_native.scene.SceneNode_onFinished_set(this, typeof v === 'function' ? v : 0); });
    accessor(SceneNode.prototype, "onAnimationEnd", function () { return this._onAnimationEnd; }, function (v) { this._onAnimationEnd = v; __bro_native.scene.SceneNode_onAnimationEnd_set(this, typeof v === 'function' ? v : 0); });
    accessor(SceneNode.prototype, "renderPriority", function () { return __bro_native.scene.SceneNode_renderPriority_get(this); }, function (v) { __bro_native.scene.SceneNode_renderPriority_set(this, v | 0); });
    accessor(SceneNode.prototype, "emissionStrength", function () { return __bro_native.scene.SceneNode_emissionStrength_get(this); }, function (v) { __bro_native.scene.SceneNode_emissionStrength_set(this, +v); });
    accessor(SceneNode.prototype, "upperFade", function () { return __bro_native.scene.SceneNode_upperFade_get(this); }, function (v) { __bro_native.scene.SceneNode_upperFade_set(this, +v); });
    accessor(SceneNode.prototype, "lowerFade", function () { return __bro_native.scene.SceneNode_lowerFade_get(this); }, function (v) { __bro_native.scene.SceneNode_lowerFade_set(this, +v); });
    accessor(SceneNode.prototype, "normalFade", function () { return __bro_native.scene.SceneNode_normalFade_get(this); }, function (v) { __bro_native.scene.SceneNode_normalFade_set(this, +v); });
    accessor(SceneNode.prototype, "modulate", function () { return Array.from(__bro_native.scene.SceneNode_modulate_get(this)); }, function (v) {
        const c = parseColor(v);
        const arr = Array.isArray(c) ? (c.length === 3 ? [c[0], c[1], c[2], 1.0] : c) : [1, 1, 1, 1];
        __bro_native.scene.SceneNode_modulate_set(this, toF64(arr));
    });
    accessor(SceneNode.prototype, "shadowBias",
        function () { return __bro_native.scene.SceneNode_shadowBias_get(this); },
        function (v) { __bro_native.scene.SceneNode_shadowBias_set(this, +v); });
    accessor(SceneNode.prototype, "shadowNormalBias",
        function () { return __bro_native.scene.SceneNode_shadowNormalBias_get(this); },
        function (v) { __bro_native.scene.SceneNode_shadowNormalBias_set(this, +v); });
    accessor(SceneNode.prototype, "cascadeCount", function () { return __bro_native.scene.SceneNode_cascadeCount_get(this); }, function (v) { __bro_native.scene.SceneNode_cascadeCount_set(this, v | 0); });
    accessor(SceneNode.prototype, "cascadeSplitLambda", function () { return __bro_native.scene.SceneNode_cascadeSplitLambda_get(this); }, function (v) { __bro_native.scene.SceneNode_cascadeSplitLambda_set(this, +v); });
    accessor(SceneNode.prototype, "fov", function () { return __bro_native.scene.SceneNode_type_get(this) === 'camera' ? __bro_native.scene.SceneNode_fov_get(this) : undefined; }, function (v) { if (__bro_native.scene.SceneNode_type_get(this) === 'camera') __bro_native.scene.SceneNode_fov_set(this, +v); });
    accessor(SceneNode.prototype, "near", function () { return __bro_native.scene.SceneNode_type_get(this) === 'camera' ? __bro_native.scene.SceneNode_near_get(this) : undefined; }, function (v) { if (__bro_native.scene.SceneNode_type_get(this) === 'camera') __bro_native.scene.SceneNode_near_set(this, +v); });
    accessor(SceneNode.prototype, "far", function () { return __bro_native.scene.SceneNode_type_get(this) === 'camera' ? __bro_native.scene.SceneNode_far_get(this) : undefined; }, function (v) { if (__bro_native.scene.SceneNode_type_get(this) === 'camera') __bro_native.scene.SceneNode_far_set(this, +v); });
    accessor(SceneNode.prototype, "projection", function () { return __bro_native.scene.SceneNode_type_get(this) === 'camera' ? __bro_native.scene.SceneNode_projection_get(this) : undefined; }, function (v) { if (__bro_native.scene.SceneNode_type_get(this) === 'camera') __bro_native.scene.SceneNode_projection_set(this, String(v)); });
    accessor(SceneNode.prototype, "aspect", function () { return __bro_native.scene.SceneNode_type_get(this) === 'camera' ? __bro_native.scene.SceneNode_aspect_get(this) : undefined; }, function (v) { if (__bro_native.scene.SceneNode_type_get(this) === 'camera') __bro_native.scene.SceneNode_aspect_set(this, +v); });
    accessor(SceneNode.prototype, "orthoHeight", function () { return __bro_native.scene.SceneNode_type_get(this) === 'camera' ? __bro_native.scene.SceneNode_orthoHeight_get(this) : undefined; }, function (v) { if (__bro_native.scene.SceneNode_type_get(this) === 'camera') __bro_native.scene.SceneNode_orthoHeight_set(this, +v); });
    accessor(SceneNode.prototype, "size", function () { return __bro_native.scene.SceneNode_type_get(this) === 'camera' ? __bro_native.scene.SceneNode_orthoHeight_get(this) : undefined; }, function (v) { if (__bro_native.scene.SceneNode_type_get(this) === 'camera') __bro_native.scene.SceneNode_orthoHeight_set(this, +v); });
    fn(SceneNode.prototype, "setMaterial", function setMaterial(opts) {
        if (opts) {
            if (opts.metallic !== undefined) this.metallic = opts.metallic;
            if (opts.roughness !== undefined) this.roughness = opts.roughness;
            if (opts.emissive !== undefined) this.emissive = opts.emissive;
        }
        return this;
    });

    // ---- bro.scene.SceneGraph ------------------------------------------------
    function SceneGraph() {
        throw new TypeError("bro.scene.SceneGraph is not constructible: instances come from the natives that return one");
    }
    {
        const proto = __bro_native.scene.SceneGraphProto;
        if (proto === undefined) throw new Error("bro.scene.SceneGraph: native class prototype not published (registerNatives_scene did not run)");
        Object.setPrototypeOf(proto, SceneGraph.prototype);
    }
    fn(mount(bro, "scene"), "SceneGraph", SceneGraph);
    globalThis.SceneGraph = SceneGraph;
    accessor(SceneGraph.prototype, "root", function () { const r = __bro_native.scene.SceneGraph_root_get(this); return r || undefined; }, undefined);
    accessor(SceneGraph.prototype, "cameraX", function () { return __bro_native.scene.SceneGraph_cameraX_get(this); }, function (v) { __bro_native.scene.SceneGraph_cameraX_set(this, v); });
    accessor(SceneGraph.prototype, "cameraY", function () { return __bro_native.scene.SceneGraph_cameraY_get(this); }, function (v) { __bro_native.scene.SceneGraph_cameraY_set(this, v); });
    accessor(SceneGraph.prototype, "cameraZoom", function () { return __bro_native.scene.SceneGraph_cameraZoom_get(this); }, function (v) { __bro_native.scene.SceneGraph_cameraZoom_set(this, v); });
    accessor(SceneGraph.prototype, "showLightIcons", function () { return __bro_native.scene.SceneGraph_showLightIcons_get(this); }, function (v) { __bro_native.scene.SceneGraph_showLightIcons_set(this, v); });
    accessor(SceneGraph.prototype, "frustumCulling", function () { return __bro_native.scene.SceneGraph_frustumCulling_get(this); }, function (v) { __bro_native.scene.SceneGraph_frustumCulling_set(this, v); });
    accessor(SceneGraph.prototype, "shadowCache", function () { return __bro_native.scene.SceneGraph_shadowCache_get(this); }, function (v) { __bro_native.scene.SceneGraph_shadowCache_set(this, v); });
    accessor(SceneGraph.prototype, "renderScale", function () { return __bro_native.scene.SceneGraph_renderScale_get(this); }, function (v) { __bro_native.scene.SceneGraph_renderScale_set(this, v); });
    accessor(SceneGraph.prototype, "msaa", function () { return __bro_native.scene.SceneGraph_msaa_get(this); }, function (v) { __bro_native.scene.SceneGraph_msaa_set(this, v); });
    accessor(SceneGraph.prototype, "activeCamera", function () { return __bro_native.scene.SceneGraph_activeCamera_get(this); }, function (v) { if (v === null || v === undefined) __bro_native.scene.SceneGraph_clearActiveCamera(this); else __bro_native.scene.SceneGraph_activeCamera_set(this, v); });

    accessor(SceneGraph.prototype, "viewMatrix",
        function () {
            if (!__bro_native.scene.SceneGraph_root_get(this)) return null;
            return Array.from(__bro_native.scene.SceneGraph_viewMatrix_get(this));
        },
        undefined);
    accessor(SceneGraph.prototype, "projectionMatrix",
        function () {
            if (!__bro_native.scene.SceneGraph_root_get(this)) return null;
            return Array.from(__bro_native.scene.SceneGraph_projectionMatrix_get(this));
        },
        undefined);
    accessor(SceneGraph.prototype, "cameraEye",
        function () {
            if (!__bro_native.scene.SceneGraph_root_get(this)) return null;
            return Array.from(__bro_native.scene.SceneGraph_cameraEye_get(this));
        },
        undefined);
    fn(SceneGraph.prototype, "createNode", function createNode(opts) {
        if (!__bro_native.scene.SceneGraph_root_get(this)) return undefined;
        if (typeof opts === 'string') opts = { name: opts };
        const d_opts = opts === undefined ? {} : opts;
        return __bro_native.scene.SceneGraph_createNode(this, d_opts.name !== undefined, d_opts.name === undefined ? '' : d_opts.name, d_opts.position === undefined ? EMPTY_F64 : toF64(d_opts.position), d_opts.rotation === undefined ? EMPTY_F64 : toF64(d_opts.rotation), d_opts.scale === undefined ? EMPTY_F64 : toF64(d_opts.scale), d_opts.visible !== undefined, d_opts.visible === undefined ? false : d_opts.visible);
    });
    fn(SceneGraph.prototype, "createHtmlNode", function createHtmlNode(opts) {
        const d_opts = opts === undefined ? {} : opts;
        return __bro_native.scene.SceneGraph_createHtmlNode(this, d_opts.html !== undefined, d_opts.html === undefined ? '' : d_opts.html, d_opts.width !== undefined, d_opts.width === undefined ? 0 : d_opts.width, d_opts.height !== undefined, d_opts.height === undefined ? 0 : d_opts.height, d_opts.position === undefined ? EMPTY_F64 : toF64(d_opts.position), d_opts.rotation === undefined ? EMPTY_F64 : toF64(d_opts.rotation), d_opts.scale === undefined ? EMPTY_F64 : toF64(d_opts.scale), d_opts.visible !== undefined, d_opts.visible === undefined ? false : d_opts.visible);
    });
    fn(SceneGraph.prototype, "createLight", function createLight(opts) {
        const d_opts = opts === undefined ? {} : opts;
        let color = d_opts.color;
        const innerCone = d_opts.innerCone !== undefined ? d_opts.innerCone : d_opts.innerAngle;
        const outerCone = d_opts.outerCone !== undefined ? d_opts.outerCone : d_opts.outerAngle;
        const castShadow = d_opts.castShadow !== undefined ? d_opts.castShadow : d_opts.castsShadow;
        const node = __bro_native.scene.SceneGraph_createLight(this,
            d_opts.type !== undefined, d_opts.type === undefined ? '' : d_opts.type,
            color === undefined ? EMPTY_F64 : toF64(color),
            d_opts.intensity !== undefined, d_opts.intensity === undefined ? 0 : d_opts.intensity,
            d_opts.range !== undefined, d_opts.range === undefined ? 0 : d_opts.range,
            innerCone !== undefined, innerCone === undefined ? 0 : innerCone,
            outerCone !== undefined, outerCone === undefined ? 0 : outerCone,
            castShadow !== undefined, castShadow === undefined ? false : castShadow,
            d_opts.position === undefined ? EMPTY_F64 : toF64(d_opts.position),
            d_opts.rotation === undefined ? EMPTY_F64 : toF64(d_opts.rotation));
        if (node) {
            applyNodeOpts(node, d_opts);
            if (d_opts.direction !== undefined) node.direction = d_opts.direction;
            if (d_opts.innerAngle !== undefined) node.innerAngle = d_opts.innerAngle;
            if (d_opts.outerAngle !== undefined) node.outerAngle = d_opts.outerAngle;
            if (d_opts.shadowBias !== undefined) node.shadowBias = d_opts.shadowBias;
            if (d_opts.shadowNormalBias !== undefined) node.shadowNormalBias = d_opts.shadowNormalBias;
            if (d_opts.cascadeCount !== undefined) node.cascadeCount = d_opts.cascadeCount;
            if (d_opts.cascadeSplitLambda !== undefined) node.cascadeSplitLambda = d_opts.cascadeSplitLambda;
        }
        return node;
    });

    fn(SceneGraph.prototype, "createParticles", function createParticles(opts) {
        const d_opts = opts === undefined ? {} : opts;
        return __bro_native.scene.SceneGraph_createParticles(this, d_opts.maxParticles !== undefined, d_opts.maxParticles === undefined ? 0 : d_opts.maxParticles, d_opts.texture !== undefined, d_opts.texture === undefined ? '' : d_opts.texture, d_opts.position === undefined ? EMPTY_F64 : toF64(d_opts.position), d_opts.visible !== undefined, d_opts.visible === undefined ? false : d_opts.visible);
    });
    fn(SceneGraph.prototype, "createDecal", function createDecal(opts) {
        const d_opts = opts === undefined ? {} : opts;
        let sz = d_opts.size;
        if (typeof sz === 'number') sz = [sz, sz, sz];
        const texStr = typeof d_opts.texture === 'string' ? d_opts.texture : '';
        const node = __bro_native.scene.SceneGraph_createDecal(this, texStr !== '', texStr, sz === undefined ? EMPTY_F64 : toF64(sz), d_opts.position === undefined ? EMPTY_F64 : toF64(d_opts.position), d_opts.rotation === undefined ? EMPTY_F64 : toF64(d_opts.rotation));
        if (node && opts) {
            applyNodeOpts(node, d_opts);
            if (d_opts.size !== undefined) {
                if (typeof d_opts.size === 'number') node.scale = [d_opts.size, d_opts.size, d_opts.size];
                else node.scale = d_opts.size;
            }
            if (opts.modulate !== undefined) node.modulate = opts.modulate;
            if (opts.upperFade !== undefined) node.upperFade = opts.upperFade;
            if (opts.lowerFade !== undefined) node.lowerFade = opts.lowerFade;
            if (opts.normalFade !== undefined) node.normalFade = opts.normalFade;
            if (opts.renderPriority !== undefined) node.renderPriority = opts.renderPriority;
            if (opts.emissionStrength !== undefined) node.emissionStrength = opts.emissionStrength;
            if (opts.texture && typeof opts.texture === 'object') node.setBaseColorTexture(opts.texture);
            if (opts.emissionTexture && typeof opts.emissionTexture === 'object') node.setEmissionTexture(opts.emissionTexture);
            if (opts.name !== undefined) node.name = opts.name;
        }
        return node;
    });
    fn(SceneGraph.prototype, "createReflectionProbe", function createReflectionProbe(opts) {
        const d_opts = opts === undefined ? {} : opts;
        let sz = d_opts.size;
        if (typeof sz === 'number') sz = [sz, sz, sz];
        const node = __bro_native.scene.SceneGraph_createReflectionProbe(this, sz === undefined ? EMPTY_F64 : toF64(sz), d_opts.resolution !== undefined, d_opts.resolution === undefined ? 0 : d_opts.resolution, d_opts.position === undefined ? EMPTY_F64 : toF64(d_opts.position));
        if (node) {
            applyNodeOpts(node, d_opts);
            if (d_opts.size !== undefined) {
                if (typeof d_opts.size === 'number') node.scale = [d_opts.size, d_opts.size, d_opts.size];
                else node.scale = d_opts.size;
            }
        }
        return node;
    });

    fn(SceneGraph.prototype, "createTween", function createTween() {
        if (!__bro_native.scene.SceneGraph_root_get(this)) return undefined;
        return __bro_native.scene.SceneGraph_createTween(this);
    });
    fn(SceneGraph.prototype, "createAnimationPlayer", function createAnimationPlayer() {
        return __bro_native.scene.SceneGraph_createAnimationPlayer(this);
    });
    fn(SceneGraph.prototype, "createTerrain", function createTerrain(opts) {
        const d_opts = opts === undefined ? {} : opts;
        const d_opts_noise = d_opts.noise === undefined ? {} : d_opts.noise;
        return __bro_native.scene.SceneGraph_createTerrain(this, d_opts.chunkSize === undefined ? EMPTY_I32 : toI32(d_opts.chunkSize), d_opts.cellSize !== undefined, d_opts.cellSize === undefined ? 0 : d_opts.cellSize, d_opts.loadRadius !== undefined, d_opts.loadRadius === undefined ? 0 : d_opts.loadRadius, d_opts.unloadRadius !== undefined, d_opts.unloadRadius === undefined ? 0 : d_opts.unloadRadius, d_opts.maxLoadsPerUpdate !== undefined, d_opts.maxLoadsPerUpdate === undefined ? 0 : d_opts.maxLoadsPerUpdate, d_opts.seed !== undefined, d_opts.seed === undefined ? 0 : d_opts.seed, d_opts_noise.frequency !== undefined, d_opts_noise.frequency === undefined ? 0 : d_opts_noise.frequency, d_opts_noise.octaves !== undefined, d_opts_noise.octaves === undefined ? 0 : d_opts_noise.octaves, d_opts_noise.gain !== undefined, d_opts_noise.gain === undefined ? 0 : d_opts_noise.gain, d_opts_noise.lacunarity !== undefined, d_opts_noise.lacunarity === undefined ? 0 : d_opts_noise.lacunarity, d_opts.baseHeight !== undefined, d_opts.baseHeight === undefined ? 0 : d_opts.baseHeight, d_opts.heightAmplitude !== undefined, d_opts.heightAmplitude === undefined ? 0 : d_opts.heightAmplitude, d_opts.seaLevel !== undefined, d_opts.seaLevel === undefined ? 0 : d_opts.seaLevel, d_opts.meshMode !== undefined, d_opts.meshMode === undefined ? 0 : d_opts.meshMode, d_opts.terraceStep !== undefined, d_opts.terraceStep === undefined ? 0 : d_opts.terraceStep, d_opts.continentFrequency !== undefined, d_opts.continentFrequency === undefined ? 0 : d_opts.continentFrequency, d_opts.continentMin !== undefined, d_opts.continentMin === undefined ? 0 : d_opts.continentMin, d_opts.continentMax !== undefined, d_opts.continentMax === undefined ? 0 : d_opts.continentMax, d_opts.mountainFrequency !== undefined, d_opts.mountainFrequency === undefined ? 0 : d_opts.mountainFrequency, d_opts.mountainAmplitude !== undefined, d_opts.mountainAmplitude === undefined ? 0 : d_opts.mountainAmplitude, d_opts.mountainOctaves !== undefined, d_opts.mountainOctaves === undefined ? 0 : d_opts.mountainOctaves, d_opts.lodLevels !== undefined, d_opts.lodLevels === undefined ? 0 : d_opts.lodLevels, d_opts.lodScaleFactor !== undefined, d_opts.lodScaleFactor === undefined ? 0 : d_opts.lodScaleFactor, d_opts.planetRadius !== undefined, d_opts.planetRadius === undefined ? 0 : d_opts.planetRadius, d_opts.origin === undefined ? EMPTY_F64 : toF64(d_opts.origin), d_opts.palette === undefined ? EMPTY_F32 : toF32(d_opts.palette));
    });
    fn(SceneGraph.prototype, "createClipmapTerrain", function createClipmapTerrain(opts) {
        const d_opts = opts === undefined ? {} : opts;
        return __bro_native.clipmap.createClipmapTerrain(this, d_opts.levels !== undefined, d_opts.levels === undefined ? 0 : d_opts.levels, d_opts.resolution !== undefined, d_opts.resolution === undefined ? 0 : d_opts.resolution, d_opts.cellSize !== undefined, d_opts.cellSize === undefined ? 0 : d_opts.cellSize, d_opts.heightScale !== undefined, d_opts.heightScale === undefined ? 0 : d_opts.heightScale, d_opts.seaLevel !== undefined, d_opts.seaLevel === undefined ? 0 : d_opts.seaLevel, d_opts.snowLine !== undefined, d_opts.snowLine === undefined ? 0 : d_opts.snowLine, d_opts.maxCellScale !== undefined, d_opts.maxCellScale === undefined ? 0 : d_opts.maxCellScale, d_opts.planetRadius !== undefined, d_opts.planetRadius === undefined ? 0 : d_opts.planetRadius, d_opts.layerFade !== undefined, d_opts.layerFade === undefined ? false : d_opts.layerFade, d_opts.coverageFloor !== undefined, d_opts.coverageFloor === undefined ? false : d_opts.coverageFloor, d_opts.cubicSurface !== undefined, d_opts.cubicSurface === undefined ? false : d_opts.cubicSurface, d_opts.cubicHeight !== undefined, d_opts.cubicHeight === undefined ? false : d_opts.cubicHeight, d_opts.detailWavelength !== undefined, d_opts.detailWavelength === undefined ? 0 : d_opts.detailWavelength, d_opts.detailRelief !== undefined, d_opts.detailRelief === undefined ? 0 : d_opts.detailRelief, d_opts.detailGain !== undefined, d_opts.detailGain === undefined ? 0 : d_opts.detailGain, d_opts.detailOctaves !== undefined, d_opts.detailOctaves === undefined ? 0 : d_opts.detailOctaves);
    });
    fn(SceneGraph.prototype, "createTileWorld", function createTileWorld(opts) {
        opts = opts || {};
        // Any typed-array view (a canvas's Uint8ClampedArray included) or a
        // bare ArrayBuffer travels as a plain array; JSON would otherwise
        // turn it into an index-keyed object the native cannot read.
        const json = JSON.stringify(opts, (k, v) => {
            if (ArrayBuffer.isView(v) && !(v instanceof DataView)) return Array.from(v);
            if (v instanceof ArrayBuffer) return Array.from(new Uint8Array(v));
            return v;
        });
        const res = __bro_native.tile_world.createTileWorldJson(this, json);
        if (!res) throw new Error("createTileWorld: invalid configuration");
        return res;
    });
    fn(SceneGraph.prototype, "findById", function findById(id) {
        if (id === undefined) throw new TypeError("bro.scene.SceneGraph.prototype.findById: id is required");
        return __bro_native.scene.SceneGraph_findById(this, id);
    });
    fn(SceneGraph.prototype, "findByName", function findByName(name) {
        if (name === undefined) throw new TypeError("bro.scene.SceneGraph.prototype.findByName: name is required");
        return __bro_native.scene.SceneGraph_findByName(this, name);
    });
    fn(SceneGraph.prototype, "destroyNode", function destroyNode(node) {
        if (node === undefined) throw new TypeError("bro.scene.SceneGraph.prototype.destroyNode: node is required");
        __bro_native.scene.SceneGraph_destroyNode(this, node);
    });
    fn(SceneGraph.prototype, "setCamera", function setCamera(opts) {
        const d_opts = opts === undefined ? {} : opts;
        const eye = d_opts.eye !== undefined ? d_opts.eye : d_opts.position;
        const target = d_opts.target !== undefined ? d_opts.target : d_opts.lookAt;
        __bro_native.scene.SceneGraph_setCamera(this, d_opts.fov !== undefined, d_opts.fov === undefined ? 0 : d_opts.fov, d_opts.near !== undefined, d_opts.near === undefined ? 0 : d_opts.near, d_opts.far !== undefined, d_opts.far === undefined ? 0 : d_opts.far, eye === undefined ? EMPTY_F64 : toF64(eye), target === undefined ? EMPTY_F64 : toF64(target), d_opts.up === undefined ? EMPTY_F64 : toF64(d_opts.up), d_opts.aspect !== undefined, d_opts.aspect === undefined ? 0 : d_opts.aspect, d_opts.quaternion === undefined ? EMPTY_F64 : toF64(d_opts.quaternion), d_opts.mode !== undefined, d_opts.mode === undefined ? '' : d_opts.mode, d_opts.size !== undefined, d_opts.size === undefined ? 0 : d_opts.size);
    });
    fn(SceneGraph.prototype, "createCamera", function createCamera(opts) {
        const d_opts = opts === undefined ? {} : opts;
        const eye = d_opts.eye !== undefined ? d_opts.eye : d_opts.position;
        const target = d_opts.target !== undefined ? d_opts.target : d_opts.lookAt;
        // `projection` / `orthoHeight` are the node-attribute spellings of
        // `mode` / `size`; either form reaches the native as mode/size.
        const mode = d_opts.mode !== undefined ? d_opts.mode : d_opts.projection;
        const size = d_opts.size !== undefined ? d_opts.size : d_opts.orthoHeight;
        const cam = __bro_native.scene.SceneGraph_createCamera(this, d_opts.fov !== undefined, d_opts.fov === undefined ? 0 : d_opts.fov, d_opts.near !== undefined, d_opts.near === undefined ? 0 : d_opts.near, d_opts.far !== undefined, d_opts.far === undefined ? 0 : d_opts.far, eye === undefined ? EMPTY_F64 : toF64(eye), target === undefined ? EMPTY_F64 : toF64(target), d_opts.up === undefined ? EMPTY_F64 : toF64(d_opts.up), d_opts.aspect !== undefined, d_opts.aspect === undefined ? 0 : d_opts.aspect, d_opts.quaternion === undefined ? EMPTY_F64 : toF64(d_opts.quaternion), mode !== undefined, mode === undefined ? '' : mode, size !== undefined, size === undefined ? 0 : size);
        if (d_opts.name !== undefined) cam.name = d_opts.name;
        if (d_opts.active) this.setActiveCamera(cam);
        return cam;
    });
    fn(SceneGraph.prototype, "setActiveCamera", function setActiveCamera(camera) {
        if (camera === undefined) throw new TypeError("bro.scene.SceneGraph.prototype.setActiveCamera: camera is required");
        if (camera === null) {
            __bro_native.scene.SceneGraph_clearActiveCamera(this);
            return;
        }
        __bro_native.scene.SceneGraph_setActiveCamera(this, camera);
    });
    fn(SceneGraph.prototype, "setToneMap", function setToneMap(opts) {
        const d_opts = opts === undefined ? {} : opts;
        const wp = d_opts.whitePoint !== undefined ? d_opts.whitePoint : d_opts.gamma;
        __bro_native.scene.SceneGraph_setToneMap(this, d_opts.mode !== undefined, d_opts.mode === undefined ? '' : d_opts.mode, d_opts.exposure !== undefined, d_opts.exposure === undefined ? 0 : d_opts.exposure, wp !== undefined, wp === undefined ? 0 : wp);
    });
    fn(SceneGraph.prototype, "setAmbient", function setAmbient(opts, g, b) {
        let col = [0.03, 0.03, 0.03];
        if (typeof opts === 'number' && typeof g === 'number' && typeof b === 'number') {
            col = [opts, g, b];
        } else if (Array.isArray(opts) || ArrayBuffer.isView(opts)) {
            col = opts;
        } else if (opts && opts.color) {
            col = opts.color;
        }
        __bro_native.scene.SceneGraph_setAmbient(this, toF64(col), false, 0);
    });
    fn(SceneGraph.prototype, "setWind", function setWind(opts, speed) {
        // setWind({ direction, strength, frequency }); the (dir, strength)
        // positional spelling the generated docs carried for a while maps
        // onto the same natives.
        let d_opts = opts === undefined || opts === null ? {} : opts;
        if (Array.isArray(opts) || ArrayBuffer.isView(opts)) d_opts = { direction: opts, strength: speed };
        __bro_native.scene.SceneGraph_setWind(this, d_opts.direction === undefined ? EMPTY_F64 : toF64(d_opts.direction), d_opts.strength !== undefined, d_opts.strength === undefined ? 0 : d_opts.strength, d_opts.frequency !== undefined, d_opts.frequency === undefined ? 0 : d_opts.frequency);
    });
    fn(SceneGraph.prototype, "setShadowQuality", function setShadowQuality(opts, pcfTaps) {
        // setShadowQuality({ atlasSize, pcfTaps }) or the positional
        // setShadowQuality(atlasSize, pcfTaps) every workshop app calls.
        let d_opts = opts === undefined || opts === null ? {} : opts;
        if (typeof opts === "number") d_opts = { atlasSize: opts, pcfTaps: pcfTaps };
        __bro_native.scene.SceneGraph_setShadowQuality(this, d_opts.atlasSize !== undefined, d_opts.atlasSize === undefined ? 0 : d_opts.atlasSize, d_opts.pcfTaps !== undefined, d_opts.pcfTaps === undefined ? 0 : d_opts.pcfTaps);
    });
    fn(SceneGraph.prototype, "setShadowCache", function setShadowCache(opts) {
        const d_opts = opts === undefined ? {} : opts;
        __bro_native.scene.SceneGraph_setShadowCache(this, d_opts.enabled !== undefined, d_opts.enabled === undefined ? false : d_opts.enabled, d_opts.staticResolution !== undefined, d_opts.staticResolution === undefined ? 0 : d_opts.staticResolution);
    });
    fn(SceneGraph.prototype, "setFog", function setFog(opts) {
        const d_opts = opts === undefined ? {} : opts;
        const f_start = d_opts.startDistance !== undefined ? d_opts.startDistance : d_opts.start;
        __bro_native.scene.SceneGraph_setFog(this, d_opts.mode !== undefined, d_opts.mode === undefined ? '' : d_opts.mode, d_opts.color === undefined ? EMPTY_F64 : toF64(d_opts.color), d_opts.density !== undefined, d_opts.density === undefined ? 0 : d_opts.density, f_start !== undefined, f_start === undefined ? 0 : f_start, d_opts.end !== undefined, d_opts.end === undefined ? 0 : d_opts.end, d_opts.heightFalloff !== undefined, d_opts.heightFalloff === undefined ? 0 : d_opts.heightFalloff, d_opts.height !== undefined, d_opts.height === undefined ? 0 : d_opts.height);
    });
    fn(SceneGraph.prototype, "setAtmosphere", function setAtmosphere(opts) {
        const d_opts = opts === undefined ? {} : opts;
        __bro_native.scene.SceneGraph_setAtmosphere(this, d_opts.rayleigh === undefined ? EMPTY_F64 : toF64(d_opts.rayleigh), d_opts.mie === undefined ? EMPTY_F64 : toF64(d_opts.mie), d_opts.turbidity !== undefined, d_opts.turbidity === undefined ? 0 : d_opts.turbidity, d_opts.sunPosition === undefined ? EMPTY_F64 : toF64(d_opts.sunPosition), d_opts.sunIntensity !== undefined, d_opts.sunIntensity === undefined ? 0 : d_opts.sunIntensity);
    });
    fn(SceneGraph.prototype, "setStarfield", function setStarfield(opts) {
        const d_opts = opts === undefined ? {} : opts;
        __bro_native.scene.SceneGraph_setStarfield(this, d_opts.starCount !== undefined, d_opts.starCount === undefined ? 0 : d_opts.starCount, d_opts.starSize !== undefined, d_opts.starSize === undefined ? 0 : d_opts.starSize, d_opts.twinkleSpeed !== undefined, d_opts.twinkleSpeed === undefined ? 0 : d_opts.twinkleSpeed, d_opts.tint === undefined ? EMPTY_F64 : toF64(d_opts.tint));
    });
    fn(SceneGraph.prototype, "setTiltShift", function setTiltShift(opts) {
        const d_opts = opts === undefined ? {} : opts;
        __bro_native.scene.SceneGraph_setTiltShift(this, d_opts.blur !== undefined, d_opts.blur === undefined ? 0 : d_opts.blur, d_opts.focus !== undefined, d_opts.focus === undefined ? 0 : d_opts.focus, d_opts.range !== undefined, d_opts.range === undefined ? 0 : d_opts.range);
    });
    fn(SceneGraph.prototype, "setBloom", function setBloom(opts) {
        const d_opts = opts === undefined ? {} : opts;
        __bro_native.scene.SceneGraph_setBloom(this, d_opts.threshold !== undefined, d_opts.threshold === undefined ? 0 : d_opts.threshold, d_opts.intensity !== undefined, d_opts.intensity === undefined ? 0 : d_opts.intensity, d_opts.radius !== undefined, d_opts.radius === undefined ? 0 : d_opts.radius, d_opts.iterations !== undefined, d_opts.iterations === undefined ? 0 : d_opts.iterations);
    });
    fn(SceneGraph.prototype, "setSSAO", function setSSAO(opts) {
        const d_opts = opts === undefined ? {} : opts;
        __bro_native.scene.SceneGraph_setSSAO(this, d_opts.radius !== undefined, d_opts.radius === undefined ? 0 : d_opts.radius, d_opts.bias !== undefined, d_opts.bias === undefined ? 0 : d_opts.bias, d_opts.intensity !== undefined, d_opts.intensity === undefined ? 0 : d_opts.intensity, d_opts.sampleCount !== undefined, d_opts.sampleCount === undefined ? 0 : d_opts.sampleCount);
    });
    fn(SceneGraph.prototype, "setSSR", function setSSR(opts) {
        if (!opts || opts.enabled === false) {
            __bro_native.scene.SceneGraph_setSSR(this, false, 0, false, 0, false, 0, false, 0);
            return;
        }
        const d_opts = opts;
        const maxDist = d_opts.maxDistance !== undefined ? d_opts.maxDistance : 30;
        const step = d_opts.stepCount !== undefined ? d_opts.stepCount : 48;
        const thick = d_opts.thickness !== undefined ? d_opts.thickness : 0.3;
        const rough = d_opts.roughnessCutoff !== undefined ? d_opts.roughnessCutoff : 0.05;
        __bro_native.scene.SceneGraph_setSSR(this, true, maxDist, true, thick, true, step, true, rough);
    });
    fn(SceneGraph.prototype, "setDepthOfField", function setDepthOfField(opts) {
        const d_opts = opts === undefined ? {} : opts;
        __bro_native.scene.SceneGraph_setDepthOfField(this, d_opts.focusDistance !== undefined, d_opts.focusDistance === undefined ? 0 : d_opts.focusDistance, d_opts.focalLength !== undefined, d_opts.focalLength === undefined ? 0 : d_opts.focalLength, d_opts.fStop !== undefined, d_opts.fStop === undefined ? 0 : d_opts.fStop, d_opts.maxBlur !== undefined, d_opts.maxBlur === undefined ? 0 : d_opts.maxBlur);
    });
    fn(SceneGraph.prototype, "setColorLUT", function setColorLUT(opts) {
        if (!opts) return __bro_native.scene.SceneGraph_setColorLUT(this, '', 0, 0);
        const p = opts.path || opts.texture || '';
        const s = opts.size !== undefined ? opts.size : 0;
        const a = opts.amount !== undefined ? opts.amount : (opts.intensity !== undefined ? opts.intensity : 1.0);
        return __bro_native.scene.SceneGraph_setColorLUT(this, p, s, a);
    });
    fn(SceneGraph.prototype, "setFXAA", function setFXAA(enabled) {
        if (enabled === undefined) throw new TypeError("bro.scene.SceneGraph.prototype.setFXAA: enabled is required");
        __bro_native.scene.SceneGraph_setFXAA(this, enabled);
    });
    fn(SceneGraph.prototype, "setRenderScale", function setRenderScale(scale) {
        if (scale === undefined) throw new TypeError("bro.scene.SceneGraph.prototype.setRenderScale: scale is required");
        __bro_native.scene.SceneGraph_setRenderScale(this, scale);
    });
    fn(SceneGraph.prototype, "setMSAA", function setMSAA(samples) {
        if (samples === undefined) throw new TypeError("bro.scene.SceneGraph.prototype.setMSAA: samples is required");
        __bro_native.scene.SceneGraph_setMSAA(this, samples);
    });
    fn(SceneGraph.prototype, "setEnvironment", function setEnvironment(opts) {
        if (opts === null || opts === undefined) {
            __bro_native.scene.SceneGraph_setEnvironment(this, true, '', false, '', EMPTY_F64, false, 0, false, 0, false, false);
            return true;
        }
        if (typeof opts !== 'object') return false;
        const d_opts = opts;
        const pano = d_opts.panorama !== undefined ? d_opts.panorama : d_opts.hdr;
        const rot = d_opts.rotation;
        __bro_native.scene.SceneGraph_setEnvironment(this,
            pano !== undefined, pano === undefined ? '' : pano,
            d_opts.cubeMap !== undefined, d_opts.cubeMap === undefined ? '' : d_opts.cubeMap,
            d_opts.color === undefined ? EMPTY_F64 : toF64(d_opts.color),
            d_opts.intensity !== undefined, d_opts.intensity === undefined ? 0 : d_opts.intensity,
            rot !== undefined, rot === undefined ? 0 : rot,
            d_opts.background !== undefined, d_opts.background === undefined ? false : d_opts.background);
        return true;
    });
    fn(SceneGraph.prototype, "setFrustumCulling", function setFrustumCulling(e) { if (e === undefined) throw new TypeError("bro.scene.SceneGraph.prototype.setFrustumCulling: enabled is required"); __bro_native.scene.SceneGraph_setFrustumCulling(this, e); });
    fn(SceneGraph.prototype, "cullStats", function cullStats() {
        if (!__bro_native.scene.SceneGraph_root_get(this)) return null;
        const s = __bro_native.scene.SceneGraph_cullStatsJson(this);
        return s ? JSON.parse(s) : { totalNodes: 0, renderedNodes: 0, culledNodes: 0 };
    });
    fn(SceneGraph.prototype, "clear", function clear() { __bro_native.scene.SceneGraph_clear(this); });
    fn(SceneGraph.prototype, "syncPhysics", function syncPhysics() { __bro_native.scene.SceneGraph_syncPhysics(this); });
    fn(SceneGraph.prototype, "raycast", function raycast(origin, direction, maxDist) {
        if (origin === undefined) throw new TypeError("bro.scene.SceneGraph.prototype.raycast: origin is required");
        if (direction === undefined) throw new TypeError("bro.scene.SceneGraph.prototype.raycast: direction is required");
        if (!__bro_native.scene.SceneGraph_raycast(this, toF64(origin), toF64(direction))) return null;
        const dist = __bro_native.scene.SceneGraph_raycast_distance();
        if (maxDist !== undefined && dist > maxDist) return null;
        const inst = __bro_native.scene.SceneGraph_raycast_instance();
        const res = {
            hit: true,
            node: __bro_native.scene.SceneGraph_raycast_node(),
            point: Array.from(__bro_native.scene.SceneGraph_raycast_point()),
            normal: Array.from(__bro_native.scene.SceneGraph_raycast_normal()),
            distance: dist
        };
        if (inst >= 0) res.instance = inst;
        return res;
    });

    fn(SceneGraph.prototype, "unprojectLocal", function unprojectLocal(x, y) {
        if (x === undefined || y === undefined) throw new TypeError("bro.scene.SceneGraph.prototype.unprojectLocal: x and y are required");
        const r = __bro_native.scene.SceneGraph_unprojectLocal(this, +x, +y);
        if (r.length < 6) return null;
        return { origin: [r[0], r[1], r[2]], dir: [r[3], r[4], r[5]] };
    });
    fn(SceneGraph.prototype, "projectLocal", function projectLocal(x, y, z) {
        if (Array.isArray(x) || ArrayBuffer.isView(x)) { z = x[2]; y = x[1]; x = x[0]; }
        if (x === undefined || y === undefined || z === undefined) throw new TypeError("bro.scene.SceneGraph.prototype.projectLocal: x, y and z are required");
        const r = __bro_native.scene.SceneGraph_projectLocal(this, +x, +y, +z);
        if (r.length < 3) return null;
        return { x: r[0], y: r[1], depth: r[2], behind: !(r[2] > 1e-6) };
    });
    fn(SceneGraph.prototype, "bindAudioListenerToCamera", function bindAudioListenerToCamera(bind) {
        if (bind === undefined) throw new TypeError("bro.scene.SceneGraph.prototype.bindAudioListenerToCamera: bind is required");
        __bro_native.scene.SceneGraph_bindAudioListenerToCamera(this, bind);
    });
    // attachAIWorld is installed from C++ on the native SceneGraph prototype
    // (installSceneGraphAgent, host_scene_agent.cpp).
    fn(SceneGraph.prototype, "detachAIWorld", function detachAIWorld() {
        __bro_native.scene.SceneGraph_detachAIWorld(this);
    });

    fn(SceneGraph.prototype, "createMesh", function createMesh(opts) {
        if (!__bro_native.scene.SceneGraph_root_get(this)) return undefined;
        if (typeof opts === 'string') opts = { mesh: opts };
        const meshObj = (opts && opts.mesh && typeof opts.mesh !== 'string') ? opts.mesh : null;
        const node = __bro_native.scene.SceneGraph_createMesh(this, opts || {}, meshObj);
        if (!node) return undefined;
        applyNodeOpts(node, opts);
        return node;
    });
    fn(SceneGraph.prototype, "createShape", function createShape(opts) {
        const node = __bro_native.scene.SceneGraph_createShape(this, opts ? JSON.stringify(opts) : "");
        if (node) applyNodeOpts(node, opts);
        return node;
    });
    fn(SceneGraph.prototype, "createSprite", function createSprite(opts) {
        const node = __bro_native.scene.SceneGraph_createSprite(this, opts ? JSON.stringify(opts) : "");
        if (node) applyNodeOpts(node, opts);
        return node;
    });
    fn(SceneGraph.prototype, "createPhysicsNode", function createPhysicsNode(opts) {
        const node = __bro_native.scene.SceneGraph_createPhysicsNode(this, opts ? JSON.stringify(opts) : "");
        if (node) applyNodeOpts(node, opts);
        return node;
    });
    fn(SceneGraph.prototype, "createParticles3D", function createParticles3D(opts) {
        const node = __bro_native.scene.SceneGraph_createParticles3D(this, opts ? JSON.stringify(opts) : "");
        if (node) {
            applyNodeOpts(node, opts);
            if (opts && typeof opts.onFinished === 'function') node.onFinished = opts.onFinished;
        }
        return node;
    });
    fn(SceneGraph.prototype, "createGaussianSplat", function createGaussianSplat(opts) {
        if (!__bro_native.scene.SceneGraph_root_get(this)) return undefined;
        const node = __bro_native.scene.SceneGraph_createGaussianSplat(this, "");
        if (!node) return undefined;
        if (opts) {
            if (typeof opts === 'string') {
                __bro_native.scene.SceneNode_loadSplatPly(node, opts);
            } else {
                if (opts.path) __bro_native.scene.SceneNode_loadSplatPly(node, opts.path);
                if (opts.cloud) node.setCloud(opts.cloud);
                applyNodeOpts(node, opts);
            }
        }
        return node;
    });
    fn(SceneGraph.prototype, "createSkinnedMesh", function createSkinnedMesh(opts) {
        if (!__bro_native.scene.SceneGraph_root_get(this)) return undefined;
        const meshObj = (opts && opts.mesh && typeof opts.mesh !== 'string') ? opts.mesh : null;
        const node = __bro_native.scene.SceneGraph_createSkinnedMesh(this, opts || {}, meshObj);
        if (!node) return undefined;
        applyNodeOpts(node, opts);
        return node;
    });
    fn(SceneGraph.prototype, "createInstancedMesh", function createInstancedMesh(opts) {
        if (!__bro_native.scene.SceneGraph_root_get(this)) return undefined;
        const meshObj = (opts && opts.mesh && typeof opts.mesh !== 'string') ? opts.mesh : null;
        const node = __bro_native.scene.SceneGraph_createInstancedMesh(this, opts || {}, meshObj);
        if (!node) return undefined;
        applyNodeOpts(node, opts);
        if (opts) {
            if (opts.instances) node.setInstances(opts.instances);
            if (opts.instancesFromTransforms) node.setInstancesFromTransforms(opts.instancesFromTransforms);
        }
        return node;
    });
    fn(SceneGraph.prototype, "asTexture", function asTexture() {
        const scn = this;
        return { _scene: scn, get valid() { return __bro_native.scene.SceneGraph_isValid(scn); } };
    });

    accessor(SceneNode.prototype, "hasShader", function () { return __bro_native.scene.SceneNode_hasShader(this); }, undefined);
    fn(SceneNode.prototype, "setShader", function setShader(opts) {
        if (!opts || typeof opts !== 'object') throw new TypeError("setShader expects an options object");
        if (opts.vertex !== undefined && typeof opts.vertex !== 'string') throw new TypeError("vertex must be a string");
        if (opts.fragment !== undefined && typeof opts.fragment !== 'string') throw new TypeError("fragment must be a string");
        if (!opts.vertex && !opts.fragment) throw new TypeError("at least one of vertex or fragment must be provided");
        if (opts.uniforms && typeof opts.uniforms === 'object') {
            for (const k of Object.keys(opts.uniforms)) {
                if (!k.startsWith('u_')) throw new TypeError("Uniform names must start with 'u_'");
                const v = opts.uniforms[k];
                if (typeof v !== 'number' && !Array.isArray(v) && !ArrayBuffer.isView(v)) throw new TypeError("Uniform values must be numeric");
            }
        }
        const err = __bro_native.scene.SceneNode_setShader(this, opts.vertex || "", opts.fragment || "", opts.uniforms ? JSON.stringify(opts.uniforms) : "");
        if (err && err.length > 0) throw new Error(err);
        return this;
    });
    fn(SceneNode.prototype, "clearShader", function clearShader() { __bro_native.scene.SceneNode_clearShader(this); return this; });
    fn(SceneNode.prototype, "setShaderUniform", function setShaderUniform(name, val) {
        if (!name || typeof name !== 'string' || !name.startsWith('u_')) throw new TypeError("Uniform names must start with 'u_'");
        if (typeof val === 'number') {
            __bro_native.scene.SceneNode_setShaderUniform(this, name, new Float64Array([val]));
        } else if (Array.isArray(val) || ArrayBuffer.isView(val)) {
            for (let i = 0; i < val.length; i++) {
                if (typeof val[i] !== 'number') throw new TypeError("Uniform values must be numeric");
            }
            __bro_native.scene.SceneNode_setShaderUniform(this, name, toF64(val));
        } else {
            throw new TypeError("Uniform values must be numeric");
        }
        return this;
    });
    fn(SceneNode.prototype, "setLodMeshes", function setLodMeshes(lods) {
        const json = Array.isArray(lods) ? JSON.stringify(lods.map(l => ({
            maxDist: l.maxDist || l.distance || 0,
            meshPositions: l.mesh ? Array.from(l.mesh.positions || []) : [],
            meshNormals: l.mesh ? Array.from(l.mesh.normals || []) : [],
            meshIndices: l.mesh ? Array.from(l.mesh.indices || []) : []
        }))) : "[]";
        __bro_native.scene.SceneNode_setLodMeshes(this, json);
        return this;
    });
    // updateMesh(mesh, opts): a Mesh or { positions, indices, normals?, uvs?,
    // colors?, tangents? }; the native reads the members and recomputes
    // normals when opts.recomputeNormals is set or none were given.
    fn(SceneNode.prototype, "updateMesh", function updateMesh(mesh, opts) {
        if (mesh === undefined) throw new TypeError("bro.scene.SceneNode.prototype.updateMesh: mesh is required");
        __bro_native.scene.SceneNode_updateMesh(this, mesh, !!(opts && opts.recomputeNormals));
        return this;
    });
    accessor(SceneNode.prototype, "lodCount", function () { return __bro_native.scene.SceneNode_lodCount(this); }, undefined);
    accessor(SceneNode.prototype, "lodLevel", function () { return __bro_native.scene.SceneNode_lodLevel(this); }, undefined);
    fn(SceneNode.prototype, "setVisibilityRange", function setVisibilityRange(begin, end, margin) {
        __bro_native.scene.SceneNode_visibilityRange_set(this, begin, end, margin || 0);
        return this;
    });
    fn(SceneNode.prototype, "clearVisibilityRange", function clearVisibilityRange() {
        __bro_native.scene.SceneNode_visibilityRange_clear(this);
        return this;
    });
    accessor(SceneNode.prototype, "visibilityRange", function () {
        const r = __bro_native.scene.SceneNode_visibilityRange_get(this);
        return (r && r.length >= 3) ? { begin: r[0], end: r[1], margin: r[2] } : null;
    }, function (v) {
        if (!v) __bro_native.scene.SceneNode_visibilityRange_clear(this);
        else __bro_native.scene.SceneNode_visibilityRange_set(this, v.begin || 0, v.end || 0, v.margin || 0);
    });
    fn(SceneNode.prototype, "setInstances", function setInstances(data) {
        const f32 = toF32(data);
        if (f32.length % 16 === 0) __bro_native.scene.SceneNode_setInstances(this, f32);
        else if (f32.length % 9 === 0) __bro_native.scene.SceneNode_setInstancesFromTransforms(this, f32);
        else __bro_native.scene.SceneNode_setInstances(this, f32);
        return this;
    });
    fn(SceneNode.prototype, "setInstancesFromTransforms", function setInstancesFromTransforms(data) {
        __bro_native.scene.SceneNode_setInstancesFromTransforms(this, toF32(data));
        return this;
    });
    accessor(SceneNode.prototype, "instanceCount", function () { return __bro_native.scene.SceneNode_instanceCount_get(this); }, undefined);
    // RGBA bytes of an { data, width, height } image. A view keeps its own
    // window of the buffer (ImageData.data, a subarray), not the whole buffer.
    function rgbaBytes(d) {
        if (d instanceof Uint8Array) return d;
        if (ArrayBuffer.isView(d)) return new Uint8Array(d.buffer, d.byteOffset, d.byteLength);
        return new Uint8Array(d);
    }
    fn(SceneNode.prototype, "setBaseColorTexture", function setBaseColorTexture(src) {
        if (!src) { __bro_native.scene.SceneNode_clearBaseColorTexture(this); return this; }
        const scn = src._scene || (src instanceof SceneGraph ? src : null);
        if (scn) { __bro_native.scene.SceneNode_setBaseColorTextureFromScene(this, scn); return this; }
        if (src.data && src.width && src.height) {
            __bro_native.scene.SceneNode_setBaseColorTextureData(this, src.width, src.height, rgbaBytes(src.data));
            return this;
        }
        return this;
    });
    fn(SceneNode.prototype, "setEmissionTexture", function setEmissionTexture(src) {
        if (src && src.data && src.width && src.height) {
            __bro_native.scene.SceneNode_setEmissionTextureData(this, src.width, src.height, rgbaBytes(src.data));
        } else if (!src) {
            __bro_native.scene.SceneNode_setEmissionTextureData(this, 0, 0, new Uint8Array(0));
        }
        return this;
    });
    fn(SceneNode.prototype, "setShaderTexture", function setShaderTexture(name, opts) {
        if (!name || !opts) return this;
        const isSub = opts.x !== undefined || opts.y !== undefined;
        const x = opts.x || 0, y = opts.y || 0, w = opts.width || 0, h = opts.height || 0;
        const data = opts.data ? (opts.data instanceof Float32Array ? opts.data : Float32Array.from(opts.data)) : EMPTY_F32;
        __bro_native.scene.SceneNode_setShaderTexture(this, name, x, y, w, h, data, !!opts.mipmap, isSub);
        return this;
    });
    fn(SceneNode.prototype, "capture", function capture() {
        __bro_native.scene.SceneNode_probeCapture(this);
        return this;
    });
    accessor(SceneNode.prototype, "updateMode", function () { return __bro_native.scene.SceneNode_updateMode_get(this); }, function (v) { __bro_native.scene.SceneNode_updateMode_set(this, v); });
    accessor(SceneNode.prototype, "resolution", function () { return __bro_native.scene.SceneNode_resolution_get(this); }, function (v) { __bro_native.scene.SceneNode_resolution_set(this, v); });
    accessor(SceneNode.prototype, "boxProjection", function () { return __bro_native.scene.SceneNode_boxProjection_get(this); }, function (v) { __bro_native.scene.SceneNode_boxProjection_set(this, v); });

    fn(SceneGraph.prototype, "toImageData", function toImageData() {
        const cw = __bro_native.scene.SceneGraph_canvasWidth(this);
        const ch = __bro_native.scene.SceneGraph_canvasHeight(this);
        const buf = __bro_native.scene.SceneGraph_readTonemapPixels(this);
        if (!buf || buf.length === 0) return null;
        const scale = __bro_native.scene.SceneGraph_renderScale_get(this) || 1.0;
        const w = Math.round(cw * scale), h = Math.round(ch * scale);
        const data = new Uint8ClampedArray(buf.buffer, buf.byteOffset, buf.byteLength);
        if (typeof globalThis.ImageData !== 'undefined') {
            try { return new globalThis.ImageData(data, w, h); } catch (e) {}
        }
        return { width: w, height: h, data };
    });
    fn(SceneGraph.prototype, "captureFrame", function captureFrame(width, height) {
        if (width !== undefined && height !== undefined) {
            __bro_native.scene.SceneGraph_setCanvasSize(this, width, height);
        }
        __bro_native.scene.SceneGraph_render(this);
        const cw = width !== undefined ? width : __bro_native.scene.SceneGraph_canvasWidth(this);
        const ch = height !== undefined ? height : __bro_native.scene.SceneGraph_canvasHeight(this);
        const buf = __bro_native.scene.SceneGraph_readTonemapPixels(this);
        if (!buf || buf.length === 0) return null;
        const scale = __bro_native.scene.SceneGraph_renderScale_get(this) || 1.0;
        const w = Math.round(cw * scale), h = Math.round(ch * scale);
        const data = new Uint8ClampedArray(buf.buffer, buf.byteOffset, buf.byteLength);
        if (typeof globalThis.ImageData !== 'undefined') {
            try { return new globalThis.ImageData(data, w, h); } catch (e) {}
        }
        return { width: w, height: h, data };
    });
    // No setClearColor: the scene clears to transparent and the canvas's CSS
    // background shows through (docs/scene-api.js). The method that was here
    // dropped its colour and only reset the environment intensity to 1.
})();
