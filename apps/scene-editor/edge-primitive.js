// =============================================================================
// EdgePrimitive — leaf SceneObject for orphan edges (Line tool exits without
// closure, Arc tool's open-arc output, future skeletal sketches).
//
// Carries a vertex array + a list of (a, b) edge pairs in LOCAL space. Renders
// through the same EdgeMesh prism pipeline used for face primitive silhouettes
// (one scene-graph node, one draw call). Holds no triangle geometry, no
// face-groups, no BVH — picking via raycast is intentionally not supported in
// this MVP (selection still works through the outliner).
//
// Inference: contributes endpoint + on-edge + midpoint snaps so the user can
// snap-draw onto previously placed lines. Built directly here without going
// through buildInferenceGeo (which expects faceGroups/triangles).
// =============================================================================

(function (global) {
    'use strict';

    const M = Mat4Lib;
    const DEFAULT_THICKNESS = 0.012;
    const DEFAULT_COLOR     = [0.17, 0.24, 0.31, 1.0];

    function EdgePrimitive(opts) {
        opts = opts || {};
        opts.kind = 'edge-primitive';
        global.SceneObject.call(this, opts);

        this.scene         = opts.scene;
        this.color         = opts.color || '#ffa502';
        this.edgeThickness = opts.edgeThickness != null ? opts.edgeThickness : DEFAULT_THICKNESS;
        this.edgeColor     = opts.edgeColor || _hexToRgba(this.color);

        this.positions = null;          // Float32Array, LOCAL xyz interleaved
        this.edges     = null;          // [{a, b}, ...] indices into positions

        this.inferenceGeo        = null;
        this._worldInferenceGeo  = null;
        this._worldInferenceDirty = true;

        this.edgesNode = null;

        if (opts.positions && opts.edges) {
            this._install(opts.positions, opts.edges);
        }
    }
    EdgePrimitive.prototype = Object.create(global.SceneObject.prototype);
    EdgePrimitive.prototype.constructor = EdgePrimitive;

    EdgePrimitive.prototype._onWorldInvalidated = function () {
        this._worldInferenceDirty = true;
        this._applyTransformToNode();
    };

    EdgePrimitive.prototype._onVisibilityChanged = function () {
        if (this.edgesNode) this.edgesNode.visible = this.isEffectivelyVisible();
    };

    EdgePrimitive.prototype._install = function (positions, edges) {
        this.positions = positions instanceof Float32Array
            ? positions : new Float32Array(positions);
        this.edges = _normalizeEdges(edges);
        this.inferenceGeo = _buildEdgeInferenceGeo(this.positions, this.edges);
        this._worldInferenceGeo = null;
        this._worldInferenceDirty = true;
        this._rebuildEdges();
    };

    // Replace geometry in place. Used by tools that mutate an existing
    // edge primitive (extend, simplify, etc.).
    EdgePrimitive.prototype.updateGeometry = function (positions, edges) {
        this._install(positions, edges);
    };

    EdgePrimitive.prototype._rebuildEdges = function () {
        if (this.edgesNode) {
            this.edgesNode.destroy();
            this.edgesNode = null;
        }
        if (!this.edges || this.edges.length === 0) return;
        const data = EdgeMesh.buildEdgeMesh(this.positions, this.edges, {
            thickness: this.edgeThickness,
            color:     this.edgeColor,
        });
        this.edgesNode = this.scene.createMesh({
            positions: data.positions,
            normals:   data.normals,
            colors:    data.colors,
            indices:   data.indices,
            unlit:     true,
            name:      this.name + '-edges',
        });
        this.edgesNode.visible = this.isEffectivelyVisible();
        this._applyTransformToNode();
    };

    EdgePrimitive.prototype._applyTransformToNode = function () {
        if (!this.edgesNode) return;
        const w = this.getWorldMatrix();
        const dec = M.decomposeTRS(w);
        const eul = M.quatToEuler(dec.rotation);
        const n = this.edgesNode;
        n.x = dec.translation[0]; n.y = dec.translation[1]; n.z = dec.translation[2];
        n.rotationX = eul[0]; n.rotationY = eul[1]; n.rotationZ = eul[2];
        n.scaleX = dec.scale[0]; n.scaleY = dec.scale[1]; n.scaleZ = dec.scale[2];
    };

    // World-space inference geo, cached and rebuilt on transform invalidation.
    EdgePrimitive.prototype.getWorldInferenceGeo = function () {
        if (!this._worldInferenceDirty && this._worldInferenceGeo) {
            return this._worldInferenceGeo;
        }
        const w = this.getWorldMatrix();
        const lp = this.inferenceGeo.positions;
        const wp = new Float32Array(lp.length);
        for (let i = 0; i < lp.length; i += 3) {
            const p = M.transformPoint(w, [lp[i], lp[i+1], lp[i+2]]);
            wp[i] = p[0]; wp[i+1] = p[1]; wp[i+2] = p[2];
        }
        this._worldInferenceGeo = {
            positions: wp,
            vertCount: this.inferenceGeo.vertCount,
            edges:     this.inferenceGeo.edges,
        };
        this._worldInferenceDirty = false;
        return this._worldInferenceGeo;
    };

    // Edge-only — no triangle hit test. Returns null so SceneRegistry.pickAt
    // skips us cleanly.
    EdgePrimitive.prototype.raycastWorld = function () { return null; };

    EdgePrimitive.prototype.destroy = function () {
        if (this.edgesNode) { this.edgesNode.destroy(); this.edgesNode = null; }
        if (this.parent) this.parent.removeChild(this);
    };

    // ---------- helpers --------------------------------------------------

    // Edges may arrive as [{a,b}], [[a,b],...], or a flat [a,b,a,b,...]
    // typed/plain array. Normalize to [{a,b},...].
    function _normalizeEdges(edges) {
        if (!edges || edges.length === 0) return [];
        if (typeof edges[0] === 'object' && !Array.isArray(edges[0]) &&
            'a' in edges[0]) {
            return edges.map(e => ({ a: e.a | 0, b: e.b | 0 }));
        }
        if (Array.isArray(edges[0])) {
            return edges.map(p => ({ a: p[0] | 0, b: p[1] | 0 }));
        }
        // Flat list.
        const out = [];
        for (let i = 0; i + 1 < edges.length; i += 2) {
            out.push({ a: edges[i] | 0, b: edges[i + 1] | 0 });
        }
        return out;
    }

    // Build inference geo for an edge-only primitive: dedup vertex positions
    // and remap edges. Mirrors Inference.buildInferenceGeo's output shape so
    // Inference.findSnap consumes it transparently.
    const Q = 1e5;
    function _buildEdgeInferenceGeo(positions, edges) {
        const keyToIdx = new Map();
        const unique = [];
        function addVert(x, y, z) {
            const k = Math.round(x*Q)+','+Math.round(y*Q)+','+Math.round(z*Q);
            let id = keyToIdx.get(k);
            if (id === undefined) {
                id = unique.length / 3;
                keyToIdx.set(k, id);
                unique.push(x, y, z);
            }
            return id;
        }
        // Map source-vertex index → unique-vertex index.
        const srcToUnique = new Int32Array(positions.length / 3);
        for (let i = 0; i < positions.length; i += 3) {
            srcToUnique[i / 3] = addVert(positions[i], positions[i+1], positions[i+2]);
        }
        const seen = new Set();
        const outEdges = [];
        for (const e of edges) {
            const a = srcToUnique[e.a];
            const b = srcToUnique[e.b];
            if (a === b) continue;
            const k = a < b ? a + ',' + b : b + ',' + a;
            if (seen.has(k)) continue;
            seen.add(k);
            outEdges.push({ a, b });
        }
        return {
            positions: new Float32Array(unique),
            vertCount: unique.length / 3,
            edges:     outEdges,
        };
    }

    // Convert "#rrggbb" → [r, g, b, 1] floats. Falls back to dark slate on
    // parse failure so we always produce a valid color array.
    function _hexToRgba(hex) {
        if (typeof hex !== 'string') return DEFAULT_COLOR.slice();
        const m = /^#?([0-9a-f]{6})$/i.exec(hex.trim());
        if (!m) return DEFAULT_COLOR.slice();
        const v = parseInt(m[1], 16);
        return [
            ((v >> 16) & 0xff) / 255,
            ((v >> 8)  & 0xff) / 255,
            ( v        & 0xff) / 255,
            1.0,
        ];
    }

    global.EdgePrimitive = EdgePrimitive;

})(typeof globalThis !== 'undefined' ? globalThis : this);
