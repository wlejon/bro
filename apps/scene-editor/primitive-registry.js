// =============================================================================
// SceneRegistry — owns the scene-editor's SceneObject tree.
//
// The registry holds one `root` Group; every Primitive, nested Group, and
// ComponentInstance lives somewhere under it. For picking + inference the
// registry flattens the tree to the list of visible leaves (primitives +
// shadow primitives inside component instances) and runs world-space
// queries against them.
//
// Back-compat: the `primitives` property still returns the flat list of
// top-level-or-nested Primitive objects (DEPTH-FIRST) so pre-refactor code
// and tests that iterate `registry.primitives` keep working.
// =============================================================================

(function (global) {
    'use strict';

    function SceneRegistry(opts) {
        this.scene  = opts.scene;
        this.root   = new Group({ id: 0, name: '<root>' });
        this.root._registry = this;
        this.active = null;
        this._nextId = 1;
        this.onChange = null;

        // Edit context: the innermost Group whose children the user is
        // currently editing. Tools operate on primitives inside this context;
        // picking returns the outermost object-within-context for a clicked
        // leaf (SketchUp "enter group to edit its geometry" semantics).
        this.editContext = this.root;

        // Component definitions — shared subtrees referenced by
        // ComponentInstance objects. Not placed in the world tree.
        this.components = [];
    }

    SceneRegistry.prototype.nextId = function () {
        return this._nextId++;
    };

    // Flat depth-first list of every Primitive in the tree. Rebuilt on each
    // read (cheap for our sizes); cache later if it becomes a hot path.
    Object.defineProperty(SceneRegistry.prototype, 'primitives', {
        get() {
            const out = [];
            this.root.traverse(n => { if (n.kind === 'primitive') out.push(n); });
            return out;
        },
    });

    // Every top-level SceneObject directly under the root. Drives the
    // outliner tree.
    SceneRegistry.prototype.topLevel = function () {
        return this.root.children.slice();
    };

    // --- Primitive add helpers -------------------------------------------

    SceneRegistry.prototype.add = function (primitive, parent) {
        (parent || this.editContext || this.root).addChild(primitive);
        if (!this.active) this.active = primitive;
        this._emit();
        return primitive;
    };

    SceneRegistry.prototype.create = function (spec) {
        return this.createWithId(spec, this.nextId());
    };

    SceneRegistry.prototype.createWithId = function (spec, id) {
        const mesh = buildMeshFromSpec(spec);
        const prim = new Primitive({
            id,
            name:        spec.name,
            color:       spec.color,
            scene:       this.scene,
            mesh,
            translation: spec.position || spec.translation,
            rotation:    spec.rotation,
            scale:       spec.scale,
        });
        if (id >= this._nextId) this._nextId = id + 1;
        this.add(prim, spec.parent);
        return prim;
    };

    // Rebuild a Primitive from a snapshot. Restores mesh buffers + TRS.
    SceneRegistry.prototype.restoreFromSnapshot = function (snap) {
        const mesh = buildMeshFromSpec({ type: 'box', params: { sx: 1, sy: 1, sz: 1 } });
        const prim = new Primitive({
            id:    snap.id,
            name:  snap.name,
            color: snap.color,
            scene: this.scene,
            mesh,
        });
        prim.updateGeometry(snap.positions, snap.indices, snap.normals);
        if (snap.translation) prim.setTranslation(snap.translation);
        if (snap.rotation)    prim.setRotation(snap.rotation);
        if (snap.scale)       prim.setScale(snap.scale);
        prim.setVisible(snap.visible !== false);
        const parent = (snap.parentId != null ? this._findById(snap.parentId) : null)
                       || this.root;
        const idx = snap.index != null
            ? Math.max(0, Math.min(snap.index, parent.children.length))
            : parent.children.length;
        parent.children.splice(idx, 0, prim);
        prim.parent = parent;
        prim._invalidateWorld();
        if (!this.active) this.active = prim;
        if (snap.id >= this._nextId) this._nextId = snap.id + 1;
        this._emit();
        return prim;
    };

    // Build from raw mesh buffers (used by drawing tools).
    //   spec: { name, color, parent?, translation?, rotation?, scale? }
    //   meshData: { positions, indices, normals? }  (LOCAL-space)
    SceneRegistry.prototype.createFromMesh = function (spec, meshData, id) {
        const seed = buildMeshFromSpec({ type: 'box', params: { sx: 1, sy: 1, sz: 1 } });
        const prim = new Primitive({
            id,
            name:        spec.name,
            color:       spec.color,
            scene:       this.scene,
            mesh:        seed,
            translation: spec.translation || spec.position,
            rotation:    spec.rotation,
            scale:       spec.scale,
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
        (spec.parent || this.editContext || this.root).addChild(prim);
        if (!this.active) this.active = prim;
        this._emit();
        return prim;
    };

    // Build an edge-only primitive from raw vertex + edge data.
    //   spec: { name, color?, parent?, translation?, rotation?, scale? }
    //   data: { positions: Float32Array|Array,
    //           edges: [{a,b}]|[[a,b]]|flat indices }
    SceneRegistry.prototype.createEdgePrimitive = function (spec, data, id) {
        if (id == null) id = this.nextId();
        const prim = new EdgePrimitive({
            id,
            name:        spec.name,
            color:       spec.color,
            scene:       this.scene,
            positions:   data.positions,
            edges:       data.edges,
            translation: spec.translation || spec.position,
            rotation:    spec.rotation,
            scale:       spec.scale,
        });
        if (id >= this._nextId) this._nextId = id + 1;
        (spec.parent || this.editContext || this.root).addChild(prim);
        if (!this.active) this.active = prim;
        this._emit();
        return prim;
    };

    SceneRegistry.prototype.snapshotEdgePrimitive = function (prim) {
        return {
            kind:        'edge-primitive',
            id:          prim.id,
            name:        prim.name,
            color:       prim.color,
            visible:     prim.visible,
            parentId:    prim.parent && prim.parent !== this.root ? prim.parent.id : null,
            index:       prim.parent ? prim.parent.children.indexOf(prim) : -1,
            translation: prim.translation.slice(),
            rotation:    prim.rotation.slice(),
            scale:       prim.scale.slice(),
            positions:   new Float32Array(prim.positions),
            edges:       prim.edges.map(e => ({ a: e.a, b: e.b })),
        };
    };

    // Restore an edge primitive from a snapshot (history undo of delete).
    SceneRegistry.prototype.restoreEdgePrimitiveFromSnapshot = function (snap) {
        const prim = new EdgePrimitive({
            id:        snap.id,
            name:      snap.name,
            color:     snap.color,
            scene:     this.scene,
            positions: snap.positions,
            edges:     snap.edges,
        });
        if (snap.translation) prim.setTranslation(snap.translation);
        if (snap.rotation)    prim.setRotation(snap.rotation);
        if (snap.scale)       prim.setScale(snap.scale);
        prim.setVisible(snap.visible !== false);
        const parent = (snap.parentId != null ? this._findById(snap.parentId) : null)
                       || this.root;
        const idx = snap.index != null
            ? Math.max(0, Math.min(snap.index, parent.children.length))
            : parent.children.length;
        parent.children.splice(idx, 0, prim);
        prim.parent = parent;
        prim._invalidateWorld();
        if (!this.active) this.active = prim;
        if (snap.id >= this._nextId) this._nextId = snap.id + 1;
        this._emit();
        return prim;
    };

    // Capture a SceneObject subtree for history/undo. Recursive — a Group
    // snapshot contains snapshots for every child.
    SceneRegistry.prototype.snapshotPrimitive = function (prim) {
        return {
            kind:        'primitive',
            id:          prim.id,
            name:        prim.name,
            color:       prim.color,
            visible:     prim.visible,
            parentId:    prim.parent && prim.parent !== this.root ? prim.parent.id : null,
            index:       prim.parent ? prim.parent.children.indexOf(prim) : -1,
            translation: prim.translation.slice(),
            rotation:    prim.rotation.slice(),
            scale:       prim.scale.slice(),
            positions:   new Float32Array(prim.positions),
            indices:     new Uint32Array(prim.indices),
            normals:     prim.normals ? new Float32Array(prim.normals) : null,
        };
    };

    SceneRegistry.prototype.remove = function (id) {
        const obj = this._findById(id);
        if (!obj) return false;
        if (obj.destroy) obj.destroy();
        if (this.active === obj) {
            const rest = this.primitives;
            this.active = rest[0] || null;
        }
        this._emit();
        return true;
    };

    // Destroy every child of root. Does NOT reset _nextId — the project
    // loader bumps it explicitly after restoring so ids don't collide.
    SceneRegistry.prototype.clear = function () {
        for (let i = this.root.children.length - 1; i >= 0; i--) {
            const c = this.root.children[i];
            if (c.destroy) c.destroy();
        }
        this.root.children.length = 0;
        this.active = null;
        this.editContext = this.root;
        this.components = [];
        this._emit();
    };

    SceneRegistry.prototype.setActive = function (id) {
        const obj = this._findById(id);
        if (!obj || this.active === obj) return false;
        this.active = obj;
        this._emit();
        return true;
    };

    SceneRegistry.prototype.setVisible = function (id, v) {
        const obj = this._findById(id);
        if (!obj) return false;
        obj.setVisible(v);
        this._emit();
        return true;
    };

    SceneRegistry.prototype.setName = function (id, name) {
        const obj = this._findById(id);
        if (!obj) return false;
        obj.setName(name);
        this._emit();
        return true;
    };

    SceneRegistry.prototype.getById = function (id) {
        return this._findById(id);
    };

    SceneRegistry.prototype._findById = function (id) {
        let found = null;
        this.root.traverse(n => { if (!found && n.id === id) found = n; });
        return found;
    };

    // Nearest-primitive world-space raycast. Walks visible leaves under the
    // root (not just the edit context — picking sees everything but results
    // are promoted to the outermost object within the current edit context).
    SceneRegistry.prototype.pickAt = function (origin, dir, opts) {
        const excludeId = opts && opts.excludeId;
        let best = null;
        const self = this;
        this.root.traverseLeaves(function (leaf) {
            if (!leaf.isEffectivelyVisible()) return;
            if (excludeId != null && leaf.id === excludeId) return;
            if (leaf.kind !== 'primitive' && leaf.kind !== 'component-instance') return;
            // Component instances have no geometry themselves — their mirror
            // primitives are the actual leaves; traverseLeaves already yields
            // those (they have kind==='primitive'). Edge-only primitives
            // surface from traverseLeaves but skip raycast picking — they're
            // selected via the outliner.
            const hit = leaf.raycastWorld(origin, dir, 0);
            if (!hit) return;
            if (!best || hit.distance < best.hit.distance) {
                best = { primitive: leaf, hit };
            }
        });
        if (!best) return null;
        // Promote to outermost object inside the current edit context.
        const outer = best.primitive.ancestorInContext(this.editContext);
        best.object = outer;
        return best;
    };

    // Every visible leaf's world-space inference geo. Used by Inference.findSnap.
    SceneRegistry.prototype.collectInferenceGeos = function (opts) {
        const excludeId = opts && opts.excludeId;
        const out = [];
        this.root.traverseLeaves(function (leaf) {
            if (!leaf.isEffectivelyVisible()) return;
            if (excludeId != null && leaf.id === excludeId) return;
            if (leaf.kind !== 'primitive' && leaf.kind !== 'edge-primitive') return;
            out.push(leaf.getWorldInferenceGeo());
        });
        return out;
    };

    // --- Group / ungroup --------------------------------------------------

    // Wrap the given objects in a new Group parented where they currently
    // live (uses the first object's parent). World transforms preserved.
    // Returns the new Group.
    SceneRegistry.prototype.createGroup = function (members, opts) {
        if (!members || members.length === 0) return null;
        const parent = members[0].parent || this.editContext || this.root;
        const group = new Group({
            id:   this.nextId(),
            name: (opts && opts.name) || 'Group',
        });
        parent.addChild(group);
        for (const m of members) m.reparentPreservingWorld(group);
        this.active = group;
        this._emit();
        return group;
    };

    // Unwrap a Group: move each child up to the group's parent preserving
    // world transform, then destroy the Group.
    SceneRegistry.prototype.explodeGroup = function (group) {
        if (!group || group.kind !== 'group' || group === this.root) return false;
        const parent = group.parent || this.root;
        const freed = group.children.slice();
        for (const c of freed) c.reparentPreservingWorld(parent);
        if (group.destroy) group.destroy();
        if (this.active === group) this.active = freed[0] || null;
        this._emit();
        return true;
    };

    // Make a ComponentDefinition from a selection or existing Group. The
    // selected subtree is moved into the definition (re-parented under the
    // definition root), then one ComponentInstance is placed at the original
    // location pointing at the definition.
    SceneRegistry.prototype.makeComponent = function (members, opts) {
        if (!members || members.length === 0) return null;
        const parent = members[0].parent || this.editContext || this.root;
        const def = new ComponentDefinition({
            id:   this.nextId(),
            name: (opts && opts.name) || 'Component',
        });
        for (const m of members) m.reparentPreservingWorld(def.root);
        this.components.push(def);
        const inst = new ComponentInstance({
            id:         this.nextId(),
            name:       def.name,
            scene:      this.scene,
            definition: def,
        });
        parent.addChild(inst);
        this.active = inst;
        this._emit();
        return { definition: def, instance: inst };
    };

    // Insert another instance of an existing definition at `translation`.
    SceneRegistry.prototype.insertComponent = function (def, opts) {
        opts = opts || {};
        const inst = new ComponentInstance({
            id:          this.nextId(),
            name:        def.name,
            scene:       this.scene,
            definition:  def,
            translation: opts.translation,
            rotation:    opts.rotation,
            scale:       opts.scale,
        });
        (opts.parent || this.editContext || this.root).addChild(inst);
        this.active = inst;
        this._emit();
        return inst;
    };

    // --- Edit context ----------------------------------------------------

    SceneRegistry.prototype.enterContext = function (obj) {
        if (obj.kind === 'group') {
            this.editContext = obj;
        } else if (obj.kind === 'component-instance') {
            // Editing an instance opens its definition's root.
            this.editContext = obj.definition.root;
            this.editContext._editingInstance = obj;
        } else {
            return false;
        }
        this._emit();
        return true;
    };

    SceneRegistry.prototype.exitContext = function () {
        if (this.editContext === this.root) return false;
        // If we're editing a component definition, jump back to its instance's
        // parent context. Otherwise just walk up.
        if (this.editContext._editingInstance) {
            const inst = this.editContext._editingInstance;
            this.editContext._editingInstance = null;
            inst.definition.markDefinitionDirty();
            this.editContext = inst.parent || this.root;
        } else {
            this.editContext = this.editContext.parent || this.root;
        }
        this._emit();
        return true;
    };

    SceneRegistry.prototype._emit = function () {
        if (this.onChange) this.onChange();
    };

    // --- Mesh-from-spec factory ------------------------------------------

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
        throw new Error('SceneRegistry.create: unknown spec.type "' + type + '"');
    }

    global.SceneRegistry     = SceneRegistry;
    // Back-compat alias.
    global.PrimitiveRegistry = SceneRegistry;
    global.SceneRegistry.buildMeshFromSpec = buildMeshFromSpec;
    global.PrimitiveRegistry.buildMeshFromSpec = buildMeshFromSpec;

})(typeof globalThis !== 'undefined' ? globalThis : this);
