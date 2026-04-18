// =============================================================================
// SceneObject — base for the scene-editor's object hierarchy.
//
// Every entity the user can select, transform, or organize is a SceneObject:
//   - Primitive        (leaf, geometry; defined in primitive.js)
//   - Group            (container, children)
//   - ComponentInstance (leaf, references a ComponentDefinition)
//
// Each object owns a local TRS (translation + quaternion rotation + scale) and
// a parent pointer. World matrices are computed lazily from the parent chain
// and invalidated down the subtree whenever a transform changes. No scene-
// graph parenting is used for rendering — each Primitive drives its own
// meshNode via decompose(worldMatrix) so we own the composition explicitly.
//
// The registry holds the root SceneObject; tools and UI walk the tree.
// =============================================================================

(function (global) {
    'use strict';

    const M = Mat4Lib;

    // --- SceneObject ------------------------------------------------------

    function SceneObject(opts) {
        opts = opts || {};
        this.id        = opts.id != null ? opts.id : 0;
        this.name      = opts.name || ('object-' + this.id);
        this.visible   = opts.visible !== false;
        this.kind      = opts.kind || 'object';

        this.parent    = null;
        this.children  = [];

        this.translation = opts.translation ? opts.translation.slice() : [0, 0, 0];
        this.rotation    = opts.rotation    ? opts.rotation.slice()    : [0, 0, 0, 1];
        this.scale       = opts.scale       ? opts.scale.slice()       : [1, 1, 1];

        this._worldMatrix = M.identity();
        this._worldInverse = M.identity();
        this._worldDirty   = true;
        this._worldInverseDirty = true;
    }

    SceneObject.prototype.setTranslation = function (t) {
        this.translation[0] = t[0];
        this.translation[1] = t[1];
        this.translation[2] = t[2];
        this._invalidateWorld();
    };
    SceneObject.prototype.setRotation = function (q) {
        this.rotation[0] = q[0];
        this.rotation[1] = q[1];
        this.rotation[2] = q[2];
        this.rotation[3] = q[3];
        this._invalidateWorld();
    };
    SceneObject.prototype.setScale = function (s) {
        this.scale[0] = s[0];
        this.scale[1] = s[1];
        this.scale[2] = s[2];
        this._invalidateWorld();
    };
    SceneObject.prototype.setTRS = function (t, q, s) {
        this.translation[0]=t[0]; this.translation[1]=t[1]; this.translation[2]=t[2];
        this.rotation[0]=q[0]; this.rotation[1]=q[1]; this.rotation[2]=q[2]; this.rotation[3]=q[3];
        this.scale[0]=s[0]; this.scale[1]=s[1]; this.scale[2]=s[2];
        this._invalidateWorld();
    };

    // Mark this object's world matrix dirty and cascade to descendants.
    // `_onWorldInvalidated` (optional per-object hook) fires once per call for
    // subclasses that cache world-space derived data (e.g. Primitive's
    // inference cache).
    SceneObject.prototype._invalidateWorld = function () {
        if (this._worldDirty && this._worldInverseDirty) {
            // Already dirty — still fire the hook so subclasses can mark
            // their own caches (inference, etc.) even on repeat invalidation
            // during tool drags.
            if (this._onWorldInvalidated) this._onWorldInvalidated();
            for (const c of this.children) c._invalidateWorld();
            return;
        }
        this._worldDirty = true;
        this._worldInverseDirty = true;
        if (this._onWorldInvalidated) this._onWorldInvalidated();
        for (const c of this.children) c._invalidateWorld();
    };

    SceneObject.prototype.getLocalMatrix = function () {
        return M.fromTRS(this.translation, this.rotation, this.scale);
    };

    SceneObject.prototype.getWorldMatrix = function () {
        if (!this._worldDirty) return this._worldMatrix;
        const local = this.getLocalMatrix();
        if (this.parent) {
            M.multiplyInto(this._worldMatrix, this.parent.getWorldMatrix(), local);
        } else {
            this._worldMatrix.set(local);
        }
        this._worldDirty = false;
        return this._worldMatrix;
    };

    SceneObject.prototype.getWorldInverse = function () {
        if (!this._worldInverseDirty) return this._worldInverse;
        const w = this.getWorldMatrix();
        const inv = M.invertTRS(w);
        this._worldInverse.set(inv);
        this._worldInverseDirty = false;
        return this._worldInverse;
    };

    SceneObject.prototype.worldToLocalPoint = function (p) {
        return M.transformPoint(this.getWorldInverse(), p);
    };
    SceneObject.prototype.worldToLocalDir = function (d) {
        return M.transformDir(this.getWorldInverse(), d);
    };
    SceneObject.prototype.localToWorldPoint = function (p) {
        return M.transformPoint(this.getWorldMatrix(), p);
    };
    SceneObject.prototype.localToWorldDir = function (d) {
        return M.transformDir(this.getWorldMatrix(), d);
    };
    SceneObject.prototype.localToWorldNormal = function (n) {
        return M.transformNormal(this.getWorldMatrix(), n);
    };

    // --- Hierarchy --------------------------------------------------------

    SceneObject.prototype.addChild = function (child) {
        if (child === this) throw new Error('SceneObject.addChild: cycle');
        if (child.parent === this) return child;
        if (child.parent) child.parent.removeChild(child);
        child.parent = this;
        this.children.push(child);
        child._invalidateWorld();
        return child;
    };

    SceneObject.prototype.removeChild = function (child) {
        const i = this.children.indexOf(child);
        if (i < 0) return false;
        this.children.splice(i, 1);
        child.parent = null;
        child._invalidateWorld();
        return true;
    };

    // Reparent child into `newParent` while preserving its world transform.
    // The new local = inv(newParent.world) * (child.world). Needed when
    // moving primitives into/out of groups so the user doesn't see them jump.
    SceneObject.prototype.reparentPreservingWorld = function (newParent) {
        const world = new Float32Array(this.getWorldMatrix());
        if (this.parent) this.parent.removeChild(this);
        if (newParent) {
            newParent.children.push(this);
            this.parent = newParent;
        } else {
            this.parent = null;
        }
        // Rebuild local TRS so world stays the same.
        let newLocal;
        if (newParent) {
            const inv = M.invertTRS(newParent.getWorldMatrix());
            newLocal = M.multiply(inv, world);
        } else {
            newLocal = world;
        }
        const dec = M.decomposeTRS(newLocal);
        this.translation = dec.translation;
        this.rotation    = dec.rotation;
        this.scale       = dec.scale;
        this._invalidateWorld();
    };

    // Visit every Primitive in the subtree (recursive, depth-first). Groups
    // and ComponentInstances are container nodes — we descend into them and
    // yield their primitive descendants (for an instance, those are the
    // shadow primitives that mirror its definition). Used by picking +
    // inference to flatten the tree into the set of geometry it can hit.
    SceneObject.prototype.traverseLeaves = function (fn) {
        if (this.kind === 'primitive' || this.kind === 'edge-primitive') {
            fn(this); return;
        }
        for (const c of this.children) c.traverseLeaves(fn);
    };

    // Visit every object in the subtree (inclusive), pre-order.
    SceneObject.prototype.traverse = function (fn) {
        fn(this);
        for (const c of this.children) c.traverse(fn);
    };

    SceneObject.prototype.setVisible = function (v) {
        v = !!v;
        if (this.visible === v) return;
        this.visible = v;
        if (this._onVisibilityChanged) this._onVisibilityChanged();
        for (const c of this.children) {
            // Propagate a visibility refresh — children keep their own flag
            // but their effective (rendered) visibility depends on the chain.
            if (c._onVisibilityChanged) c._onVisibilityChanged();
        }
    };

    SceneObject.prototype.setName = function (name) {
        this.name = name;
    };

    // Effective visibility: any ancestor hidden ⇒ hidden.
    SceneObject.prototype.isEffectivelyVisible = function () {
        let n = this;
        while (n) {
            if (!n.visible) return false;
            n = n.parent;
        }
        return true;
    };

    // Root-most ancestor, distinct from scene root. Useful for picking in a
    // given edit context — we promote a picked leaf to the outermost wrapper
    // (group or component) within the context.
    SceneObject.prototype.ancestorInContext = function (context) {
        let n = this;
        while (n && n.parent && n.parent !== context) n = n.parent;
        return n;
    };

    // --- Group ------------------------------------------------------------
    //
    // A Group is a SceneObject that contains other SceneObjects. No geometry
    // of its own. Transforms compose through children.

    function Group(opts) {
        opts = opts || {};
        opts.kind = 'group';
        SceneObject.call(this, opts);
    }
    Group.prototype = Object.create(SceneObject.prototype);
    Group.prototype.constructor = Group;

    Group.prototype.destroy = function () {
        // Destroy children (they're owned by this group for lifetime purposes).
        for (let i = this.children.length - 1; i >= 0; i--) {
            const c = this.children[i];
            if (c.destroy) c.destroy();
        }
        this.children.length = 0;
        if (this.parent) this.parent.removeChild(this);
    };

    // --- ComponentDefinition ---------------------------------------------
    //
    // Named subtree that one or more ComponentInstance objects share. The
    // definition is effectively a "prefab": editing any instance of the
    // component (via enter-edit) mutates the definition, and all other
    // instances re-render from the shared buffers. Not a SceneObject itself
    // (it isn't placed in the world); it holds a root SceneObject subtree.

    function ComponentDefinition(opts) {
        opts = opts || {};
        this.id   = opts.id != null ? opts.id : 0;
        this.name = opts.name || ('component-' + this.id);
        // Root SceneObject for the definition's subtree. Transforms relative
        // to this root are the "component-local" transforms — instances
        // compose their own transform on top.
        this.root = new Group({ id: -this.id, name: this.name + ' (def)' });
        this.instances = [];      // back-ref for edit propagation
    }

    ComponentDefinition.prototype.addInstance = function (inst) {
        this.instances.push(inst);
    };
    ComponentDefinition.prototype.removeInstance = function (inst) {
        const i = this.instances.indexOf(inst);
        if (i >= 0) this.instances.splice(i, 1);
    };

    // Notify every instance that the definition's subtree changed. Instances
    // rebuild their rendered scene nodes from the definition.
    ComponentDefinition.prototype.markDefinitionDirty = function () {
        for (const inst of this.instances) inst._rebuildFromDefinition();
    };

    // --- ComponentInstance ------------------------------------------------
    //
    // A leaf SceneObject that, when rendered, instantiates the definition's
    // subtree under its transform. Internally it keeps a mirrored subtree of
    // "shadow" Primitives that share the definition's mesh data but track
    // their own scene node and combined world transform.

    function ComponentInstance(opts) {
        opts = opts || {};
        opts.kind = 'component-instance';
        SceneObject.call(this, opts);
        this.definition = opts.definition;   // ComponentDefinition
        this.scene      = opts.scene;        // bro SceneGraph (for nodes)
        this._shadows   = [];                // mirror Primitives (non-editable)
        if (this.definition) {
            this.definition.addInstance(this);
            this._rebuildFromDefinition();
        }
    }
    ComponentInstance.prototype = Object.create(SceneObject.prototype);
    ComponentInstance.prototype.constructor = ComponentInstance;

    ComponentInstance.prototype._clearShadows = function () {
        for (const sh of this._shadows) sh.destroy();
        this._shadows.length = 0;
    };

    // Walk the definition subtree and build a parallel "shadow" tree rooted
    // under this instance. Shadow objects mirror structure but have no
    // independent geometry — they re-use the definition primitives' mesh
    // buffers directly (buffers are copied into each shadow's render node to
    // keep lifetime simple; copies are cheap for the scene sizes we target).
    ComponentInstance.prototype._rebuildFromDefinition = function () {
        this._clearShadows();
        if (!this.definition || !this.scene) return;
        const self = this;
        const defRoot = this.definition.root;
        // For each definition-primitive leaf, create a shadow Primitive whose
        // local transform = product of (this instance world) ∘ (definition
        // chain local transforms). Simpler: build shadow's parent chain as
        // Groups mirroring the definition's chain, parented under this.
        function mirror(defNode, parentMirror) {
            if (defNode.kind === 'primitive') {
                // Build a shadow primitive sharing the mesh buffers.
                const ShadowPrim = global.Primitive;
                const shadow = ShadowPrim.createShadow({
                    source: defNode,
                    scene:  self.scene,
                });
                shadow.translation = defNode.translation.slice();
                shadow.rotation    = defNode.rotation.slice();
                shadow.scale       = defNode.scale.slice();
                parentMirror.addChild(shadow);
                self._shadows.push(shadow);
            } else {
                const g = new Group({
                    id:   -(self.id * 1000 + self._shadows.length),
                    name: defNode.name,
                });
                g.translation = defNode.translation.slice();
                g.rotation    = defNode.rotation.slice();
                g.scale       = defNode.scale.slice();
                parentMirror.addChild(g);
                for (const c of defNode.children) mirror(c, g);
            }
        }
        for (const c of defRoot.children) mirror(c, this);
    };

    ComponentInstance.prototype.destroy = function () {
        this._clearShadows();
        if (this.definition) this.definition.removeInstance(this);
        if (this.parent) this.parent.removeChild(this);
    };

    global.SceneObject        = SceneObject;
    global.Group              = Group;
    global.ComponentDefinition = ComponentDefinition;
    global.ComponentInstance  = ComponentInstance;

})(typeof globalThis !== 'undefined' ? globalThis : this);
