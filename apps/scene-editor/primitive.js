// =============================================================================
// Primitive — one editable mesh object + its derived state.
//
// Owns everything a scene-editor object needs: the source Mesh, positions/
// indices/normals buffers, the BVH (for picking), face groups (for push/pull
// + inference), inference geo (snap features), editMesh (half-edge DS), the
// render node, and the edges-overlay node. The app operates on primitives
// via the registry; the registry holds many of these.
//
// Geometry is baked into world-space positions — no per-node transform is
// used. Picking uses world-space rays against world-space BVH positions;
// adding a primitive at a nonzero position offsets the mesh vertices before
// install. Keeps ray math trivial and consistent with the original spike.
//
// After any mutation (push/pull commit), call updateGeometry(newPos, newIdx,
// newNormals) to rebuild all derived state and refresh the render nodes.
// =============================================================================

(function (global) {
    'use strict';

    const EDGE_THICKNESS = 0.01;
    const EDGE_COLOR     = [0.17, 0.24, 0.31, 1.0];

    // Face groups: maximal sets of coplanar, edge-connected triangles. Unified
    // by vertex *position* (quantized) so hard-edge seams with duplicated
    // indices still merge across the shared edge.
    //
    // `prior` (optional): a previous { groups, triToGroup } result. When
    // provided AND the tri count matches, the existing group assignments
    // are reused — only each group's normal is refreshed from current
    // positions. This preserves face identity across push/pull commits,
    // where mutating vertex positions can coincidentally make adjacent
    // face groups coplanar (e.g. pulling a cylinder facet until it aligns
    // with its neighbor). Without preservation, those facets would merge
    // into one face group, silently changing topology.
    function computeFaceGroups(positions, indices, cosTol, prior) {
        if (cosTol === undefined || cosTol === null) cosTol = 0.9995;
        const triCount = indices.length / 3;
        const normals = new Float32Array(triCount * 3);
        for (let t = 0; t < triCount; t++) {
            const i0 = indices[t * 3 + 0] * 3;
            const i1 = indices[t * 3 + 1] * 3;
            const i2 = indices[t * 3 + 2] * 3;
            const ax = positions[i1 + 0] - positions[i0 + 0];
            const ay = positions[i1 + 1] - positions[i0 + 1];
            const az = positions[i1 + 2] - positions[i0 + 2];
            const bx = positions[i2 + 0] - positions[i0 + 0];
            const by = positions[i2 + 1] - positions[i0 + 1];
            const bz = positions[i2 + 2] - positions[i0 + 2];
            let nx = ay * bz - az * by;
            let ny = az * bx - ax * bz;
            let nz = ax * by - ay * bx;
            const L = Math.hypot(nx, ny, nz) || 1;
            normals[t * 3 + 0] = nx / L;
            normals[t * 3 + 1] = ny / L;
            normals[t * 3 + 2] = nz / L;
        }

        const parent = new Int32Array(triCount);
        for (let i = 0; i < triCount; i++) parent[i] = i;
        function find(x) {
            while (parent[x] !== x) { parent[x] = parent[parent[x]]; x = parent[x]; }
            return x;
        }
        function union(a, b) {
            const ra = find(a), rb = find(b);
            if (ra !== rb) parent[rb] = ra;
        }

        const Q = 1e5;
        function posKey(vi) {
            const p = vi * 3;
            return Math.round(positions[p] * Q) + ',' +
                   Math.round(positions[p + 1] * Q) + ',' +
                   Math.round(positions[p + 2] * Q);
        }
        function edgeKey(a, b) {
            const ka = posKey(a), kb = posKey(b);
            return ka < kb ? ka + '|' + kb : kb + '|' + ka;
        }

        const edgeTris = new Map();
        for (let t = 0; t < triCount; t++) {
            for (let e = 0; e < 3; e++) {
                const ia = indices[t * 3 + e];
                const ib = indices[t * 3 + ((e + 1) % 3)];
                const k = edgeKey(ia, ib);
                const arr = edgeTris.get(k);
                if (arr) arr.push(t); else edgeTris.set(k, [t]);
            }
        }

        for (const tris of edgeTris.values()) {
            for (let i = 0; i < tris.length; i++) {
                for (let j = i + 1; j < tris.length; j++) {
                    const a = tris[i], b = tris[j];
                    const dot = normals[a * 3 + 0] * normals[b * 3 + 0] +
                                normals[a * 3 + 1] * normals[b * 3 + 1] +
                                normals[a * 3 + 2] * normals[b * 3 + 2];
                    if (dot > cosTol) union(a, b);
                }
            }
        }

        // Preservation path: prior's tri-count matches → reuse assignments
        // verbatim, just refresh each group's normal from the representative
        // triangle's current orientation.
        if (prior && prior.triToGroup && prior.triToGroup.length === triCount) {
            const triToGroup = new Int32Array(prior.triToGroup);
            const groups = prior.groups.map(g => ({
                tris: [],
                // Placeholder — overwritten below once we visit a tri in
                // this group.
                normal: [g.normal[0], g.normal[1], g.normal[2]],
            }));
            const seen = new Uint8Array(groups.length);
            for (let t = 0; t < triCount; t++) {
                const gi = triToGroup[t];
                groups[gi].tris.push(t);
                if (!seen[gi]) {
                    groups[gi].normal = [
                        normals[t * 3 + 0],
                        normals[t * 3 + 1],
                        normals[t * 3 + 2],
                    ];
                    seen[gi] = 1;
                }
            }
            return { groups, triToGroup };
        }

        const rootToIdx = new Map();
        const triToGroup = new Int32Array(triCount);
        const groups = [];
        for (let t = 0; t < triCount; t++) {
            const r = find(t);
            let gi = rootToIdx.get(r);
            if (gi === undefined) {
                gi = groups.length;
                rootToIdx.set(r, gi);
                groups.push({
                    tris: [],
                    normal: [normals[r * 3 + 0], normals[r * 3 + 1], normals[r * 3 + 2]],
                });
            }
            triToGroup[t] = gi;
            groups[gi].tris.push(t);
        }
        return { groups, triToGroup };
    }

    // Construct a Primitive from a Mesh object + metadata. The mesh is
    // assumed freshly generated and *mutable* — it gets translated by
    // `opts.position` via bromesh's Mesh.translate (which rewrites the
    // underlying C++ positions — JS-side mutation of `mesh.positions`
    // doesn't persist because the getter returns a fresh view each call).
    //
    // opts: {
    //   id, name, color, visible, scene,
    //   mesh,                     // bromesh Mesh (translated in place)
    //   position = [0,0,0],       // world-space origin for the new mesh
    //   edgeThickness, edgeColor, // optional edge-mesh tuning
    // }
    function Primitive(opts) {
        this.id       = opts.id;
        this.name     = opts.name || ('primitive-' + opts.id);
        this.color    = opts.color || '#74b9ff';
        this.visible  = opts.visible !== false;
        this.scene    = opts.scene;
        this.edgeThickness = opts.edgeThickness != null ? opts.edgeThickness : EDGE_THICKNESS;
        this.edgeColor     = opts.edgeColor || EDGE_COLOR;

        this.mesh       = null;
        this.positions  = null;
        this.indices    = null;
        this.normals    = null;
        this.bvh        = null;
        this.faceGroups = null;
        this.inferenceGeo = null;
        this.editMesh   = null;
        this.meshNode   = null;
        this.edgesNode  = null;

        if (opts.mesh) {
            const pos = opts.position;
            if (pos && (pos[0] !== 0 || pos[1] !== 0 || pos[2] !== 0)) {
                opts.mesh.translate(pos[0], pos[1], pos[2]);
            }
            this._install(opts.mesh);
        }
    }

    // First-time install: wires up the render node, BVH, face groups, edit
    // mesh, inference geo, and edges overlay.
    Primitive.prototype._install = function (mesh) {
        this.mesh      = mesh;
        this.positions = mesh.positions;
        this.indices   = mesh.indices;
        this.normals   = mesh.normals;
        this.bvh       = new MeshBVH(mesh);
        this.faceGroups   = computeFaceGroups(this.positions, this.indices);
        this.editMesh     = EditMesh.fromMeshData(this.positions, this.indices);
        this.inferenceGeo = Inference.buildInferenceGeo(
            this.positions, this.indices, this.faceGroups);
        this.meshNode = this.scene.createMesh({
            data:  mesh,
            color: this.color,
            name:  this.name,
        });
        this.meshNode.visible = this.visible;
        this._rebuildEdges();
    };

    // Build a faceGroups structure {groups, triToGroup} directly from a
    // caller-supplied triToGroup map + the geometry. Each group's normal
    // is taken from the first triangle assigned to it. Used by the push/pull
    // surgery commit, which already knows the correct grouping (bridge tris
    // explicitly assigned to merge-with-adjacent or new-wall groups) and
    // doesn't want computeFaceGroups's coplanarity-based re-grouping
    // overwriting that.
    function faceGroupsFromTriToGroup(positions, indices, triToGroup) {
        const triCount = indices.length / 3;
        if (triToGroup.length !== triCount) {
            throw new Error('faceGroupsFromTriToGroup: triToGroup length ' +
                triToGroup.length + ' != triCount ' + triCount);
        }
        // Compact group ids in case the caller used sparse ids (surgery's
        // bridge groups start at baseGroupCount and may skip values if a
        // bridge merged into an existing group).
        const idMap = new Map();
        const newToGroup = new Int32Array(triCount);
        for (let t = 0; t < triCount; t++) {
            const g = triToGroup[t];
            let nid = idMap.get(g);
            if (nid === undefined) {
                nid = idMap.size;
                idMap.set(g, nid);
            }
            newToGroup[t] = nid;
        }
        const groups = [];
        for (let i = 0; i < idMap.size; i++) {
            groups.push({ tris: [], normal: [0, 1, 0] });
        }
        const seen = new Uint8Array(groups.length);
        for (let t = 0; t < triCount; t++) {
            const gi = newToGroup[t];
            groups[gi].tris.push(t);
            if (!seen[gi]) {
                const i0 = indices[t * 3 + 0] * 3;
                const i1 = indices[t * 3 + 1] * 3;
                const i2 = indices[t * 3 + 2] * 3;
                const ax = positions[i1] - positions[i0];
                const ay = positions[i1+1] - positions[i0+1];
                const az = positions[i1+2] - positions[i0+2];
                const bx = positions[i2] - positions[i0];
                const by = positions[i2+1] - positions[i0+1];
                const bz = positions[i2+2] - positions[i0+2];
                let nx = ay*bz - az*by;
                let ny = az*bx - ax*bz;
                let nz = ax*by - ay*bx;
                const L = Math.hypot(nx, ny, nz) || 1;
                groups[gi].normal = [nx/L, ny/L, nz/L];
                seen[gi] = 1;
            }
        }
        return { groups, triToGroup: newToGroup };
    }

    // Post-mutation rebuild: push/pull commit replaces positions/indices and
    // possibly normals with fresh Float32/Uint32Arrays. All derived state
    // rebuilds; the render node is updated in place (same identity).
    //
    // `opts.priorTriToGroup` — explicit triToGroup (supplied by surgery).
    //   Used directly instead of running computeFaceGroups.
    // `opts.preserveFaceGroups` — keep existing face-group assignments when
    //   the new tri count matches the old. Used by callers that re-set
    //   positions on the same topology (legacy compatibility — surgery
    //   uses priorTriToGroup instead).
    Primitive.prototype.updateGeometry = function (positions, indices, normals, opts) {
        this.positions = positions;
        this.indices   = indices;
        if (normals) this.normals = normals;
        this.mesh.positions = positions;
        this.mesh.indices   = indices;
        if (normals) this.mesh.normals = normals;
        this.bvh = new MeshBVH(this.mesh);
        if (opts && opts.priorTriToGroup) {
            this.faceGroups = faceGroupsFromTriToGroup(
                positions, indices, opts.priorTriToGroup);
        } else {
            const prior = (opts && opts.preserveFaceGroups) ? this.faceGroups : null;
            this.faceGroups = computeFaceGroups(this.positions, this.indices, undefined, prior);
        }
        this.editMesh = EditMesh.fromMeshData(this.positions, this.indices);
        this.inferenceGeo = Inference.buildInferenceGeo(
            this.positions, this.indices, this.faceGroups);
        this.meshNode.updateMesh({
            positions, indices, normals: this.normals,
        });
        this._rebuildEdges();
    };

    // Overlay a fresh working-preview mesh during drag without mutating
    // canonical buffers. Caller still holds the working buffers.
    Primitive.prototype.previewMesh = function (positions, indices, normals) {
        this.meshNode.updateMesh({
            positions, indices, normals: normals || this.normals,
        });
    };

    // Roll the render node back to the canonical buffers (push/pull cancel).
    Primitive.prototype.revertMesh = function () {
        this.meshNode.updateMesh({
            positions: this.positions,
            indices:   this.indices,
            normals:   this.normals,
        });
    };

    Primitive.prototype._rebuildEdges = function () {
        if (this.edgesNode) {
            this.edgesNode.destroy();
            this.edgesNode = null;
        }
        if (!this.inferenceGeo.edges.length) return;
        const data = EdgeMesh.buildEdgeMesh(
            this.inferenceGeo.positions, this.inferenceGeo.edges,
            { thickness: this.edgeThickness, color: this.edgeColor });
        this.edgesNode = this.scene.createMesh({
            positions: data.positions,
            normals:   data.normals,
            colors:    data.colors,
            indices:   data.indices,
            emissive:  0.4,
            name:      this.name + '-edges',
        });
        this.edgesNode.visible = this.visible;
    };

    Primitive.prototype.setVisible = function (v) {
        v = !!v;
        if (this.visible === v) return;
        this.visible = v;
        if (this.meshNode)  this.meshNode.visible  = v;
        if (this.edgesNode) this.edgesNode.visible = v;
    };

    Primitive.prototype.setName = function (name) {
        this.name = name;
        // Node `name` isn't re-settable mid-life; purely metadata for the
        // outliner + debugging. The scene itself doesn't care.
    };

    Primitive.prototype.destroy = function () {
        if (this.meshNode)  { this.meshNode.destroy();  this.meshNode  = null; }
        if (this.edgesNode) { this.edgesNode.destroy(); this.edgesNode = null; }
    };

    // --- Editing helpers ---------------------------------------------------
    //
    // Every MeshData vertex whose quantized position matches a face-group
    // vertex must translate with the face — otherwise hard-edge seams
    // (duplicated indices at the same position across adjacent face groups)
    // tear. Returns indices into `this.positions`.
    Primitive.prototype.collectAffectedVertexIndices = function (groupIdx) {
        const tris = this.faceGroups.groups[groupIdx].tris;
        const Q = 1e5;
        const keys = new Set();
        const positions = this.positions;
        const indices = this.indices;
        function key(vi) {
            return Math.round(positions[vi * 3 + 0] * Q) + ',' +
                   Math.round(positions[vi * 3 + 1] * Q) + ',' +
                   Math.round(positions[vi * 3 + 2] * Q);
        }
        for (const t of tris) {
            for (let k = 0; k < 3; k++) {
                keys.add(key(indices[t * 3 + k]));
            }
        }
        const vertCount = positions.length / 3;
        const out = [];
        for (let vi = 0; vi < vertCount; vi++) {
            if (keys.has(key(vi))) out.push(vi);
        }
        return Uint32Array.from(out);
    };

    Primitive.prototype.faceGroupCentroid = function (groupIdx) {
        const tris = this.faceGroups.groups[groupIdx].tris;
        const P = this.positions;
        const I = this.indices;
        let cx = 0, cy = 0, cz = 0, n = 0;
        for (const t of tris) {
            for (let k = 0; k < 3; k++) {
                const vi = I[t * 3 + k];
                cx += P[vi * 3 + 0];
                cy += P[vi * 3 + 1];
                cz += P[vi * 3 + 2];
                n++;
            }
        }
        return [cx / n, cy / n, cz / n];
    };

    // Find the face group whose normal matches `n` within cos tolerance,
    // tiebreaking by centroid proximity to `ref`. Used by the VCB re-apply
    // path to re-locate the pushed face after a commit has rebuilt the
    // face-group indices.
    Primitive.prototype.findFaceGroupByNormal = function (n, ref) {
        let bestIdx = -1;
        let bestDist = Infinity;
        for (let i = 0; i < this.faceGroups.groups.length; i++) {
            const g = this.faceGroups.groups[i];
            const dot = g.normal[0]*n[0] + g.normal[1]*n[1] + g.normal[2]*n[2];
            if (dot < 0.9995) continue;
            const c = this.faceGroupCentroid(i);
            const d = Math.hypot(c[0]-ref[0], c[1]-ref[1], c[2]-ref[2]);
            if (d < bestDist) { bestDist = d; bestIdx = i; }
        }
        return bestIdx;
    };

    global.Primitive = Primitive;
    global.Primitive.computeFaceGroups = computeFaceGroups;
    global.Primitive.EDGE_THICKNESS = EDGE_THICKNESS;
    global.Primitive.EDGE_COLOR     = EDGE_COLOR;

})(typeof globalThis !== 'undefined' ? globalThis : this);
