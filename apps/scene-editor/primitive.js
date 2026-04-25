// =============================================================================
// Primitive — one editable mesh object, living as a leaf SceneObject in the
// scene-editor's hierarchy.
//
// Mesh buffers are **local-space** (relative to the primitive's TRS). Picking,
// inference, and rendering consume a lazily-computed world-space view of the
// same data, invalidated automatically whenever an ancestor transform changes.
// The render node (meshNode) and edges node are driven by the composed world
// matrix — we decompose to TRS + Euler and set the scene-node fields so bro's
// engine handles the per-frame projection.
//
// Owns: mesh buffers, BVH (local), face groups, inference geo (local +
// world cache), editMesh, polyMesh, render nodes.
//
// Topology edits (push/pull commit, line/rect/circle creation) go through
// updateGeometry(positions, indices, normals, opts) with buffers in the
// primitive's local space. Transform edits (move/rotate/scale) go through
// setTranslation/setRotation/setScale on the SceneObject base — no mesh-
// buffer mutation at all.
// =============================================================================

(function (global) {
    'use strict';

    const EDGE_THICKNESS = 0.01;
    const EDGE_COLOR     = [0.17, 0.24, 0.31, 1.0];

    // Face-group detection: maximal sets of coplanar edge-connected triangles.
    // Identity-preserving across topology-preserving mutations when `prior` is
    // supplied and the tri count matches — prevents face-group merging when
    // push/pull happens to make adjacent groups momentarily coplanar.
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

        if (prior && prior.triToGroup && prior.triToGroup.length === triCount) {
            const triToGroup = new Int32Array(prior.triToGroup);
            const groups = prior.groups.map(g => ({
                tris: [],
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

    // Primitive extends SceneObject. All fields on SceneObject (parent,
    // children, translation, rotation, scale, visible, name, id) plus:
    //   mesh, positions, indices, normals    — LOCAL-space buffers
    //   bvh, editMesh, polyMesh, faceGroups, triToFace
    //   inferenceGeo                         — LOCAL feature set
    //   _worldInferenceGeo (cached)          — world-space copy for snapping
    //   meshNode, edgesNode                  — scene-graph nodes
    //
    // opts: {
    //   id, name, color, visible, scene,
    //   mesh,                           // bromesh Mesh (LOCAL-space)
    //   translation = [0,0,0],          // initial position
    //   rotation    = [0,0,0,1],        // initial quat
    //   scale       = [1,1,1],
    //   edgeThickness, edgeColor,
    // }
    function Primitive(opts) {
        opts = opts || {};
        opts.kind = 'primitive';
        // Back-compat: older callers use `opts.position` for the initial
        // translation. Accept either; translation wins if both supplied.
        if (!opts.translation && opts.position) opts.translation = opts.position;
        global.SceneObject.call(this, opts);

        this.color         = opts.color || '#74b9ff';
        this.scene         = opts.scene;
        this.edgeThickness = opts.edgeThickness != null ? opts.edgeThickness : EDGE_THICKNESS;
        this.edgeColor     = opts.edgeColor || EDGE_COLOR;

        this.mesh         = null;
        this.positions    = null;
        this.indices      = null;
        this.normals      = null;
        this.bvh          = null;
        this.faceGroups   = null;
        this.inferenceGeo = null;
        this.editMesh     = null;
        this.polyMesh     = null;
        this.triToFace    = null;

        this.meshNode     = null;
        this.edgesNode    = null;

        this._worldInferenceGeo = null;
        this._worldInferenceDirty = true;

        // Shared-mesh mode: ComponentInstance mirror primitives point at a
        // source primitive's buffers rather than owning their own. They
        // re-use the source's faceGroups/editMesh/polyMesh/inferenceGeo
        // reference for queries; their meshNode is still independent (each
        // instance has its own transform on-screen).
        this._sharedSource = null;

        if (opts.mesh) this._install(opts.mesh);
    }
    Primitive.prototype = Object.create(global.SceneObject.prototype);
    Primitive.prototype.constructor = Primitive;

    // Any ancestor transform change invalidates our world-space inference
    // cache AND re-applies the composed world matrix to the scene node.
    Primitive.prototype._onWorldInvalidated = function () {
        this._worldInferenceDirty = true;
        this._applyTransformToNode();
    };

    Primitive.prototype._onVisibilityChanged = function () {
        const v = this.isEffectivelyVisible();
        if (this.meshNode)  this.meshNode.visible  = v;
        if (this.edgesNode) this.edgesNode.visible = v;
    };

    // First-time install. Builds all derived state from the supplied local-
    // space mesh and registers the render + edges nodes with the scene.
    Primitive.prototype._install = function (mesh) {
        this.mesh         = mesh;
        this.positions    = mesh.positions;
        this.indices      = mesh.indices;
        this.normals      = mesh.normals;
        this.bvh          = new MeshBVH(mesh);
        this.faceGroups   = computeFaceGroups(this.positions, this.indices);
        this.editMesh     = EditMesh.fromMeshData(this.positions, this.indices);
        this.polyMesh     = _buildPolyMesh(this.positions, this.indices,
                                           this.faceGroups.triToGroup);
        this.triToFace    = _buildTriToFace(this.polyMesh, this.indices,
                                            this.faceGroups.triToGroup);
        this.inferenceGeo = Inference.buildInferenceGeo(
            this.positions, this.indices, this.faceGroups);
        this._worldInferenceGeo = null;
        this._worldInferenceDirty = true;

        this.meshNode = this.scene.createMesh({
            data:  mesh,
            color: this.color,
            name:  this.name,
        });
        this.meshNode.visible = this.isEffectivelyVisible();
        this._applyTransformToNode();
        this._rebuildEdges();
    };

    // Apply the composed world matrix to our render + edges scene nodes.
    // Decomposes to TRS + Euler so bro's engine builds the same matrix back.
    Primitive.prototype._applyTransformToNode = function () {
        if (!this.meshNode) return;
        const w = this.getWorldMatrix();
        const dec = Mat4Lib.decomposeTRS(w);
        const eul = Mat4Lib.quatToEuler(dec.rotation);
        const n = this.meshNode;
        n.x = dec.translation[0]; n.y = dec.translation[1]; n.z = dec.translation[2];
        n.rotationX = eul[0]; n.rotationY = eul[1]; n.rotationZ = eul[2];
        n.scaleX = dec.scale[0]; n.scaleY = dec.scale[1]; n.scaleZ = dec.scale[2];
        if (this.edgesNode) {
            const e = this.edgesNode;
            e.x = n.x; e.y = n.y; e.z = n.z;
            e.rotationX = n.rotationX; e.rotationY = n.rotationY; e.rotationZ = n.rotationZ;
            e.scaleX = n.scaleX; e.scaleY = n.scaleY; e.scaleZ = n.scaleZ;
        }
    };

    function _buildPolyMesh(positions, indices, triToGroup) {
        if (typeof PolyMesh !== 'function') return null;
        const ttg = (triToGroup instanceof Uint32Array) ? triToGroup
                  : (triToGroup ? new Uint32Array(triToGroup) : null);
        const pm = ttg
            ? PolyMesh.fromMeshData(positions, indices, ttg)
            : PolyMesh.fromMeshData(positions, indices);
        pm.mergeFacesByGroup();
        return pm;
    }

    function _buildTriToFace(polyMesh, indices, triToGroup) {
        if (!polyMesh || !triToGroup) return null;
        const groupToFace = new Map();
        for (let f = 0; f < polyMesh.faceCount; f++) {
            const g = polyMesh.faceGroup(f);
            if (!groupToFace.has(g)) groupToFace.set(g, f);
        }
        const out = new Int32Array(indices.length / 3);
        for (let t = 0; t < out.length; t++) {
            const g = triToGroup[t];
            const f = groupToFace.get(g);
            out[t] = (f === undefined) ? -1 : f;
        }
        return out;
    }

    function faceGroupsFromTriToGroup(positions, indices, triToGroup) {
        const triCount = indices.length / 3;
        if (triToGroup.length !== triCount) {
            throw new Error('faceGroupsFromTriToGroup: triToGroup length ' +
                triToGroup.length + ' != triCount ' + triCount);
        }
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

    // Topology-changing commit. Buffers are in the primitive's LOCAL space.
    //   opts.priorTriToGroup  — explicit triToGroup (surgery commits).
    //   opts.preserveFaceGroups — reuse existing assignments when tri count
    //                              matches (legacy compat).
    //   opts.cleanRender     — re-tessellate via PolyMesh for clean concave
    //                          caps after push/pull.
    Primitive.prototype.updateGeometry = function (positions, indices, normals, opts) {
        let triToGroup;
        if (opts && opts.priorTriToGroup) {
            const fg = faceGroupsFromTriToGroup(positions, indices, opts.priorTriToGroup);
            triToGroup = fg.triToGroup;
        } else {
            const prior = (opts && opts.preserveFaceGroups) ? this.faceGroups : null;
            const fg = computeFaceGroups(positions, indices, undefined, prior);
            triToGroup = fg.triToGroup;
        }

        const polyMesh = _buildPolyMesh(positions, indices, triToGroup);
        let renderPositions = positions;
        let renderIndices   = indices;
        let renderNormals   = normals || this.normals;
        let triToFace       = null;
        let renderTriToGroup = triToGroup;

        if (polyMesh && opts && opts.cleanRender) {
            const tess = polyMesh.tessellate();
            renderPositions  = tess.positions;
            renderIndices    = tess.indices;
            renderNormals    = tess.normals;
            triToFace        = tess.triToFace;
            renderTriToGroup = tess.triToGroup;
        } else if (polyMesh) {
            triToFace = _buildTriToFace(polyMesh, indices, triToGroup);
        }

        this.positions = renderPositions;
        this.indices   = renderIndices;
        this.normals   = renderNormals;
        this.mesh.positions = renderPositions;
        this.mesh.indices   = renderIndices;
        this.mesh.normals   = renderNormals;
        this.bvh = new MeshBVH(this.mesh);
        this.faceGroups = faceGroupsFromTriToGroup(renderPositions, renderIndices,
                                                    renderTriToGroup);
        this.polyMesh   = polyMesh;
        this.triToFace  = triToFace;
        this.editMesh   = EditMesh.fromMeshData(this.positions, this.indices);
        this.inferenceGeo = Inference.buildInferenceGeo(
            this.positions, this.indices, this.faceGroups);
        this._worldInferenceDirty = true;
        this.meshNode.updateMesh({
            positions: this.positions, indices: this.indices,
            normals:   this.normals,
        });
        this._rebuildEdges();
    };

    // Live preview during tool drags — push working buffers to the render
    // node without mutating canonical state. Buffers are LOCAL-space.
    Primitive.prototype.previewMesh = function (positions, indices, normals) {
        if (!this.meshNode) return;
        this.meshNode.updateMesh({
            positions, indices, normals: normals || this.normals,
        });
    };

    Primitive.prototype.revertMesh = function () {
        if (!this.meshNode) return;
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
            unlit:     true,
            name:      this.name + '-edges',
        });
        this.edgesNode.visible = this.isEffectivelyVisible();
        this._applyTransformToNode();
    };

    Primitive.prototype.setName = function (name) {
        global.SceneObject.prototype.setName.call(this, name);
    };

    Primitive.prototype.destroy = function () {
        if (this.meshNode)  { this.meshNode.destroy();  this.meshNode  = null; }
        if (this.edgesNode) { this.edgesNode.destroy(); this.edgesNode = null; }
        if (this.parent) this.parent.removeChild(this);
    };

    // --- World-space query cache -----------------------------------------
    //
    // The snap-inference query operates on world-space points + edges. We
    // keep a local copy on the primitive; this helper yields a transformed
    // view, rebuilt only when the world matrix changes. Edge connectivity
    // stays identical — only positions are transformed.
    Primitive.prototype.getWorldInferenceGeo = function () {
        if (!this._worldInferenceGeo || this._worldInferenceDirty) {
            const local = this.inferenceGeo;
            const world = this.getWorldMatrix();
            const out = new Float32Array(local.positions.length);
            for (let i = 0; i < local.vertCount; i++) {
                const wx = world[0]*local.positions[i*3]
                         + world[4]*local.positions[i*3+1]
                         + world[8]*local.positions[i*3+2]
                         + world[12];
                const wy = world[1]*local.positions[i*3]
                         + world[5]*local.positions[i*3+1]
                         + world[9]*local.positions[i*3+2]
                         + world[13];
                const wz = world[2]*local.positions[i*3]
                         + world[6]*local.positions[i*3+1]
                         + world[10]*local.positions[i*3+2]
                         + world[14];
                out[i*3]   = wx;
                out[i*3+1] = wy;
                out[i*3+2] = wz;
            }
            this._worldInferenceGeo = {
                positions: out,
                vertCount: local.vertCount,
                edges:     local.edges,
                _owner:    this,
            };
            this._worldInferenceDirty = false;
        }
        return this._worldInferenceGeo;
    };

    // World-space BVH raycast. Transforms the ray into the primitive's local
    // space, casts against the local BVH, then transforms the hit back into
    // world space. Returns the same shape the bromesh MeshBVH yields, with
    // position + normal re-expressed in world.
    Primitive.prototype.raycastWorld = function (origin, dir, maxDist) {
        const inv = this.getWorldInverse();
        const lo = Mat4Lib.transformPoint(inv, origin);
        // For direction, transformDir is sufficient only if there's no
        // non-uniform scale — but the BVH operates on a normalized direction
        // and returns distance in local units, which we must convert back.
        const ld = Mat4Lib.transformDir(inv, dir);
        const localDirLen = Math.hypot(ld[0], ld[1], ld[2]) || 1;
        const ldNorm = [ld[0]/localDirLen, ld[1]/localDirLen, ld[2]/localDirLen];
        const localMax = maxDist != null ? maxDist * localDirLen : 0;
        const hit = this.bvh.raycast(this.mesh, lo, ldNorm, localMax);
        if (!hit) return null;
        const world = this.getWorldMatrix();
        const wp = Mat4Lib.transformPoint(world, hit.position);
        const wn = Mat4Lib.transformNormal(world, hit.normal);
        // distance in world units = local distance / localDirLen
        return {
            distance:      hit.distance / localDirLen,
            position:      wp,
            normal:        wn,
            triangleIndex: hit.triangleIndex,
            baryU: hit.baryU, baryV: hit.baryV, baryW: hit.baryW,
            // Preserve local hit for callers that need it.
            _localPosition: hit.position,
            _localNormal:   hit.normal,
        };
    };

    // --- Editing helpers (local-space) -----------------------------------

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
            for (let k = 0; k < 3; k++) keys.add(key(indices[t * 3 + k]));
        }
        const vertCount = positions.length / 3;
        const out = [];
        for (let vi = 0; vi < vertCount; vi++) {
            if (keys.has(key(vi))) out.push(vi);
        }
        return Uint32Array.from(out);
    };

    // Local-space centroid of a face group.
    Primitive.prototype.faceGroupCentroidLocal = function (groupIdx) {
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

    // World-space centroid (back-compat name). The earlier code path called
    // this `faceGroupCentroid` when positions were world-space; now it's
    // explicitly world by transforming the local centroid.
    Primitive.prototype.faceGroupCentroid = function (groupIdx) {
        return this.localToWorldPoint(this.faceGroupCentroidLocal(groupIdx));
    };

    // Bounding-box centroid in world space. Replaces the old primCentroid()
    // helper that assumed world-space positions. Called by the gizmo pivot.
    Primitive.prototype.worldCentroid = function () {
        const P = this.positions;
        let minX =  Infinity, minY =  Infinity, minZ =  Infinity;
        let maxX = -Infinity, maxY = -Infinity, maxZ = -Infinity;
        for (let i = 0; i < P.length; i += 3) {
            const x = P[i], y = P[i + 1], z = P[i + 2];
            if (x < minX) minX = x; if (x > maxX) maxX = x;
            if (y < minY) minY = y; if (y > maxY) maxY = y;
            if (z < minZ) minZ = z; if (z > maxZ) maxZ = z;
        }
        const cl = [(minX + maxX) * 0.5, (minY + maxY) * 0.5, (minZ + maxZ) * 0.5];
        return this.localToWorldPoint(cl);
    };

    // Find a face group by world-space normal + world-space reference point.
    // The VCB redo-last-pushpull path passes the saved world-space normal and
    // centroid of the previously pushed face; we transform the group's local
    // normal to world and compare. Centroid comparison is in world space for
    // the same reason.
    Primitive.prototype.findFaceGroupByNormal = function (n, ref) {
        let bestIdx = -1;
        let bestDist = Infinity;
        for (let i = 0; i < this.faceGroups.groups.length; i++) {
            const g = this.faceGroups.groups[i];
            const wn = this.localToWorldNormal(g.normal);
            const dot = wn[0]*n[0] + wn[1]*n[1] + wn[2]*n[2];
            if (dot < 0.9995) continue;
            const c = this.faceGroupCentroid(i);
            const d = Math.hypot(c[0]-ref[0], c[1]-ref[1], c[2]-ref[2]);
            if (d < bestDist) { bestDist = d; bestIdx = i; }
        }
        return bestIdx;
    };

    // --- Shared (component-instance) primitive factory -------------------
    //
    // A "shadow" primitive mirrors another primitive's mesh + BVH + groups +
    // editMesh + inferenceGeo — the data is shared, not copied — but the
    // scene node and transform are independent. Useful for component
    // instances where editing the definition auto-propagates to instances.
    Primitive.createShadow = function (opts) {
        const src = opts.source;
        const p = new Primitive({
            id:     opts.id != null ? opts.id : -src.id,
            name:   src.name,
            color:  src.color,
            scene:  opts.scene,
            translation: opts.translation,
            rotation:    opts.rotation,
            scale:       opts.scale,
        });
        p._sharedSource = src;
        p.mesh         = src.mesh;
        p.positions    = src.positions;
        p.indices      = src.indices;
        p.normals      = src.normals;
        p.bvh          = src.bvh;
        p.faceGroups   = src.faceGroups;
        p.editMesh     = src.editMesh;
        p.polyMesh     = src.polyMesh;
        p.triToFace    = src.triToFace;
        p.inferenceGeo = src.inferenceGeo;

        p.meshNode = p.scene.createMesh({
            data:  src.mesh,
            color: src.color,
            name:  src.name + '(shadow)',
        });
        p.meshNode.visible = p.isEffectivelyVisible();
        p._applyTransformToNode();
        p._rebuildEdges();
        return p;
    };

    global.Primitive = Primitive;
    global.Primitive.computeFaceGroups = computeFaceGroups;
    global.Primitive.faceGroupsFromTriToGroup = faceGroupsFromTriToGroup;
    global.Primitive.EDGE_THICKNESS = EDGE_THICKNESS;
    global.Primitive.EDGE_COLOR     = EDGE_COLOR;

})(typeof globalThis !== 'undefined' ? globalThis : this);
