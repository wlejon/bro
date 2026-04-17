// =============================================================================
// PrimitiveRegistry — owns every Primitive in the scene editor and provides
// selection + lookup + picking across them.
//
// The app queries the registry for:
//   - active primitive (target of new tool operations by default)
//   - pickAt(origin, dir): nearest visible-primitive hit under a world ray
//   - the list of primitives (for outliner rendering + multi-primitive
//     inference)
//
// Registry mutations (add/remove/setActive/setVisible/setName) invoke the
// onChange listener so UI layers like the outliner can refresh without
// polling. Inference/edges rebuild remain the responsibility of the
// individual Primitive — the registry only tracks composition.
// =============================================================================

(function (global) {
    'use strict';

    function PrimitiveRegistry(opts) {
        this.scene = opts.scene;
        this.primitives = [];
        this.active = null;
        this._nextId = 1;
        this.onChange = null;
    }

    PrimitiveRegistry.prototype.nextId = function () {
        return this._nextId++;
    };

    PrimitiveRegistry.prototype.add = function (primitive) {
        this.primitives.push(primitive);
        if (!this.active) this.active = primitive;
        this._emit();
        return primitive;
    };

    // Build + install one Primitive from a mesh-factory spec:
    //   { type: 'box'|'sphere'|'cylinder'|'plane', params, position, color,
    //     name }
    // Convenience wrapper over `new Primitive(...)`; useful for the outliner
    // "add" dropdown.
    PrimitiveRegistry.prototype.create = function (spec) {
        return this.createWithId(spec, this.nextId());
    };

    // Build + install with a caller-chosen id. Used by history redo so a
    // freshly re-added primitive keeps the same id across undo/redo cycles —
    // external references (measureBox lastOp, etc.) stay valid.
    PrimitiveRegistry.prototype.createWithId = function (spec, id) {
        const mesh = buildMeshFromSpec(spec);
        const prim = new Primitive({
            id,
            name:     spec.name,
            color:    spec.color,
            scene:    this.scene,
            mesh,
            position: spec.position,
        });
        if (id >= this._nextId) this._nextId = id + 1;
        this.add(prim);
        return prim;
    };

    // Rebuild a Primitive from a snapshot taken before a destructive
    // operation (e.g. delete). The snapshot holds raw buffers + metadata;
    // we seed a fresh Primitive with a disposable mesh, then swap in the
    // saved geometry via updateGeometry so BVH/face groups/inference all
    // rebuild correctly. Inserted at the original list index so outliner
    // order is preserved across undo/redo.
    PrimitiveRegistry.prototype.restoreFromSnapshot = function (snap) {
        const mesh = buildMeshFromSpec({ type: 'box', params: { sx: 1, sy: 1, sz: 1 } });
        const prim = new Primitive({
            id:       snap.id,
            name:     snap.name,
            color:    snap.color,
            scene:    this.scene,
            mesh,
            position: [0, 0, 0],
        });
        prim.updateGeometry(snap.positions, snap.indices, snap.normals);
        prim.setVisible(snap.visible !== false);
        const idx = Math.max(0, Math.min(snap.index != null ? snap.index : this.primitives.length,
                                         this.primitives.length));
        this.primitives.splice(idx, 0, prim);
        if (!this.active) this.active = prim;
        if (snap.id >= this._nextId) this._nextId = snap.id + 1;
        this._emit();
        return prim;
    };

    // Build and install a primitive from raw mesh buffers (vs. a
    // factory spec). Used by drawing tools where the geometry is
    // synthesized per-stroke and doesn't correspond to a primitive type.
    // `meshData` may hold Float32Array / Uint32Array or plain arrays —
    // typed views are materialized here.
    PrimitiveRegistry.prototype.createFromMesh = function (spec, meshData, id) {
        const seedMesh = buildMeshFromSpec({ type: 'box', params: { sx: 1, sy: 1, sz: 1 } });
        const prim = new Primitive({
            id,
            name:     spec.name,
            color:    spec.color,
            scene:    this.scene,
            mesh:     seedMesh,
            position: [0, 0, 0],
        });
        const pos = meshData.positions instanceof Float32Array
            ? meshData.positions : new Float32Array(meshData.positions);
        const idx = meshData.indices instanceof Uint32Array
            ? meshData.indices : new Uint32Array(meshData.indices);
        const nrm = meshData.normals
            ? (meshData.normals instanceof Float32Array
                ? meshData.normals : new Float32Array(meshData.normals))
            : null;
        prim.updateGeometry(pos, idx, nrm);
        if (id >= this._nextId) this._nextId = id + 1;
        this.primitives.push(prim);
        if (!this.active) this.active = prim;
        this._emit();
        return prim;
    };

    // Capture the current state of a primitive in a form suitable for
    // restoreFromSnapshot. Buffers are cloned so later mutations to the live
    // primitive don't bleed into history.
    PrimitiveRegistry.prototype.snapshotPrimitive = function (prim) {
        return {
            id:        prim.id,
            name:      prim.name,
            color:     prim.color,
            visible:   prim.visible,
            index:     this.primitives.indexOf(prim),
            positions: new Float32Array(prim.positions),
            indices:   new Uint32Array(prim.indices),
            normals:   prim.normals ? new Float32Array(prim.normals) : null,
        };
    };

    PrimitiveRegistry.prototype.remove = function (id) {
        const idx = this.primitives.findIndex(p => p.id === id);
        if (idx < 0) return false;
        const p = this.primitives[idx];
        p.destroy();
        this.primitives.splice(idx, 1);
        if (this.active === p) this.active = this.primitives[0] || null;
        this._emit();
        return true;
    };

    // Destroy every primitive and reset to empty. Used by project load/new
    // to wipe the scene before deserializing or seeding defaults. Does NOT
    // reset `_nextId` — callers who want that (e.g. load) set it explicitly
    // so restored ids don't collide.
    PrimitiveRegistry.prototype.clear = function () {
        for (let i = this.primitives.length - 1; i >= 0; i--) {
            this.primitives[i].destroy();
        }
        this.primitives.length = 0;
        this.active = null;
        this._emit();
    };

    PrimitiveRegistry.prototype.setActive = function (id) {
        const p = this.primitives.find(x => x.id === id);
        if (!p || this.active === p) return false;
        this.active = p;
        this._emit();
        return true;
    };

    PrimitiveRegistry.prototype.setVisible = function (id, v) {
        const p = this.primitives.find(x => x.id === id);
        if (!p) return false;
        p.setVisible(v);
        this._emit();
        return true;
    };

    PrimitiveRegistry.prototype.setName = function (id, name) {
        const p = this.primitives.find(x => x.id === id);
        if (!p) return false;
        p.setName(name);
        this._emit();
        return true;
    };

    PrimitiveRegistry.prototype.getById = function (id) {
        return this.primitives.find(x => x.id === id) || null;
    };

    // Nearest-primitive raycast. Returns { primitive, hit } or null.
    // Iterates visible primitives only — hidden primitives don't receive
    // input (matches their absence from the render). `opts.excludeId` skips
    // a specific primitive (e.g. the Move tool excludes the moving primitive
    // so the cursor can target geometry under it without self-hitting).
    PrimitiveRegistry.prototype.pickAt = function (origin, dir, opts) {
        const excludeId = opts && opts.excludeId;
        let best = null;
        for (const p of this.primitives) {
            if (!p.visible) continue;
            if (excludeId != null && p.id === excludeId) continue;
            const hit = p.bvh.raycast(p.mesh, origin, dir, 0);
            if (!hit) continue;
            if (!best || hit.distance < best.hit.distance) {
                best = { primitive: p, hit };
            }
        }
        return best;
    };

    // All visible-primitive inference geos — used by Inference.findSnap to
    // scan every visible primitive's snap features in one pass.
    // `opts.excludeId` filters out a specific primitive (Move tool uses this
    // so it doesn't snap to the moving primitive's stale pre-drag features).
    PrimitiveRegistry.prototype.collectInferenceGeos = function (opts) {
        const excludeId = opts && opts.excludeId;
        const out = [];
        for (const p of this.primitives) {
            if (!p.visible) continue;
            if (excludeId != null && p.id === excludeId) continue;
            out.push(p.inferenceGeo);
        }
        return out;
    };

    PrimitiveRegistry.prototype._emit = function () {
        if (this.onChange) this.onChange();
    };

    // --- Mesh-from-spec factory --------------------------------------------

    function buildMeshFromSpec(spec) {
        const type = spec.type;
        const p = spec.params || {};
        if (type === 'box') {
            return Mesh.box(p.sx || 1, p.sy || 1, p.sz || 1);
        }
        if (type === 'sphere') {
            return Mesh.sphere(p.r || 1, p.seg || 24, p.rings || 16);
        }
        if (type === 'cylinder') {
            return Mesh.cylinder(p.r || 1, p.h || 2, p.seg || 24);
        }
        if (type === 'plane') {
            return Mesh.plane(p.w || 2, p.d || 2, p.sx || 1, p.sz || 1);
        }
        throw new Error('PrimitiveRegistry.create: unknown spec.type "' + type + '"');
    }

    global.PrimitiveRegistry = PrimitiveRegistry;
    global.PrimitiveRegistry.buildMeshFromSpec = buildMeshFromSpec;

})(typeof globalThis !== 'undefined' ? globalThis : this);
