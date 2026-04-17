// =============================================================================
// EditMesh — half-edge topology for interactive mesh editing.
//
// bromesh's MeshData is a sink (positions + indices only); every editing tool
// (push/pull, bevel, offset, edge-split) needs adjacency. EditMesh owns that
// adjacency. Operations mutate in place; call toMeshData() to materialize a
// snapshot for rendering.
//
//   Vertex   → { x, y, z, halfEdge }     (one outgoing half-edge)
//   HalfEdge → { origin, twin, next, face }
//   Face     → { halfEdge }               (one bounding half-edge)
//
// Twins are matched by vertex *position* (quantized), so hard-edge seams
// with duplicated indices still close up. A closed manifold has a twin for
// every half-edge; boundaries are marked by twin = null.
// =============================================================================

(function (global) {
    'use strict';

    function EditMesh() {
        this.vertices  = [];
        this.halfEdges = [];
        this.faces     = [];
    }

    // --- Build from flat MeshData arrays ------------------------------------

    function fromMeshData(positions, indices, triToGroup) {
        const em = new EditMesh();

        const vertCount = positions.length / 3;
        for (let i = 0; i < vertCount; i++) {
            em.vertices.push({
                x: positions[i * 3 + 0],
                y: positions[i * 3 + 1],
                z: positions[i * 3 + 2],
                halfEdge: null,
            });
        }

        const triCount = indices.length / 3;
        for (let t = 0; t < triCount; t++) {
            const vi = [
                indices[t * 3 + 0],
                indices[t * 3 + 1],
                indices[t * 3 + 2],
            ];
            // `group` tags the face's owning face-group so surgery operations
            // can compare boundary edges across groups. Optional — defaults to
            // -1 (untagged) when no triToGroup is supplied.
            const face = {
                halfEdge: null,
                group: triToGroup ? triToGroup[t] : -1,
            };
            em.faces.push(face);
            const hes = [null, null, null];
            for (let k = 0; k < 3; k++) {
                const he = {
                    origin: em.vertices[vi[k]],
                    twin: null,
                    next: null,
                    face,
                };
                em.halfEdges.push(he);
                hes[k] = he;
                if (!em.vertices[vi[k]].halfEdge) {
                    em.vertices[vi[k]].halfEdge = he;
                }
            }
            hes[0].next = hes[1];
            hes[1].next = hes[2];
            hes[2].next = hes[0];
            face.halfEdge = hes[0];
        }

        matchTwinsByPosition(em);
        return em;
    }

    // --- Twin matching ------------------------------------------------------
    //
    // Two passes:
    //   1. By vertex index — correct for meshes that share indices across
    //      faces (UV sphere: pole vertex is duplicated *per slice*, so pass 1
    //      matches every non-pole edge but leaves pole fans unpaired).
    //   2. By quantized vertex position — closes hard-edge seams where two
    //      faces use distinct indices at the same position (Mesh.box, and
    //      also pole-adjacent half-edges in a UV sphere).

    const POS_QUANT = 1e5;

    function posKey(v) {
        return Math.round(v.x * POS_QUANT) + ',' +
               Math.round(v.y * POS_QUANT) + ',' +
               Math.round(v.z * POS_QUANT);
    }

    function matchTwinsByPosition(em) {
        // Pass 1: index-based.
        const vIdx = new Map();
        for (let i = 0; i < em.vertices.length; i++) vIdx.set(em.vertices[i], i);
        const byIdxDir = new Map();
        for (const he of em.halfEdges) {
            byIdxDir.set(vIdx.get(he.origin) + '>' + vIdx.get(he.next.origin), he);
        }
        for (const he of em.halfEdges) {
            if (he.twin) continue;
            const key = vIdx.get(he.next.origin) + '>' + vIdx.get(he.origin);
            const twin = byIdxDir.get(key);
            if (twin && twin !== he && !twin.twin) {
                he.twin = twin;
                twin.twin = he;
            }
        }

        // Pass 2: position-based closure for anything still unpaired.
        const byPosDir = new Map();
        for (const he of em.halfEdges) {
            if (he.twin) continue;
            const k = posKey(he.origin) + '|' + posKey(he.next.origin);
            // Only remember the first — if multiple unpaired half-edges share
            // the same directed position pair, the mesh is non-manifold by
            // position and we can only close one twin pair.
            if (!byPosDir.has(k)) byPosDir.set(k, he);
        }
        for (const he of em.halfEdges) {
            if (he.twin) continue;
            const k = posKey(he.next.origin) + '|' + posKey(he.origin);
            const twin = byPosDir.get(k);
            if (twin && twin !== he && !twin.twin) {
                he.twin = twin;
                twin.twin = he;
            }
        }
    }

    // --- Serialize back to MeshData -----------------------------------------
    //
    // Vertex indices are assigned from current array order. Works as long as
    // the vertex array isn't sparse; editing ops that delete vertices should
    // compact first (not needed for the spike).

    function toMeshData(em) {
        const vIdx = new Map();
        const positions = new Float32Array(em.vertices.length * 3);
        for (let i = 0; i < em.vertices.length; i++) {
            const v = em.vertices[i];
            vIdx.set(v, i);
            positions[i * 3 + 0] = v.x;
            positions[i * 3 + 1] = v.y;
            positions[i * 3 + 2] = v.z;
        }
        const indices = new Uint32Array(em.faces.length * 3);
        for (let i = 0; i < em.faces.length; i++) {
            const a = em.faces[i].halfEdge;
            const b = a.next;
            const c = b.next;
            indices[i * 3 + 0] = vIdx.get(a.origin);
            indices[i * 3 + 1] = vIdx.get(b.origin);
            indices[i * 3 + 2] = vIdx.get(c.origin);
        }
        return { positions, indices };
    }

    // --- Validation ---------------------------------------------------------
    //
    // Closed-manifold check: every half-edge has a twin, and walking .next
    // three times returns to the start on every face.

    function validate(em) {
        const errors = [];
        let boundaryCount = 0;

        for (let i = 0; i < em.halfEdges.length; i++) {
            const he = em.halfEdges[i];
            if (!he.origin) errors.push(`he[${i}].origin is null`);
            if (!he.next)   errors.push(`he[${i}].next is null`);
            if (!he.face)   errors.push(`he[${i}].face is null`);
            if (!he.twin)   boundaryCount++;
            else if (he.twin.twin !== he) {
                errors.push(`he[${i}].twin.twin !== self`);
            }
        }

        for (let i = 0; i < em.faces.length; i++) {
            const a = em.faces[i].halfEdge;
            if (!a) { errors.push(`face[${i}] has no halfEdge`); continue; }
            if (a.next.next.next !== a) {
                errors.push(`face[${i}] is not a triangle`);
            }
            if (a.face !== em.faces[i] ||
                a.next.face !== em.faces[i] ||
                a.next.next.face !== em.faces[i]) {
                errors.push(`face[${i}] half-edge face pointer mismatch`);
            }
        }

        return {
            ok: errors.length === 0,
            errors,
            boundaryHalfEdges: boundaryCount,
            isClosed: boundaryCount === 0,
        };
    }

    // --- Surgery primitives -------------------------------------------------
    //
    // The push/pull tool composes these to add geometry without warping any
    // existing face groups:
    //   1. findFaceGroupBoundary(em, gIdx) — ordered loops of HEs whose twin
    //      sits in a different face group (or null = mesh boundary).
    //   2. duplicateBoundary(em, gIdx, offset) — clone every boundary vert at
    //      `vert + offset`, rewire the group's interior triangles to use the
    //      duplicates, and detach the boundary HEs (twin = null) so they're
    //      ready for new bridge geometry.
    //   3. addBridge(em, oldHe, dupMap, bridgeGroup) — emit two triangles
    //      filling the quad (oldHe.origin → oldHe.next.origin → dup(next.origin)
    //      → dup(origin)) and assign them to `bridgeGroup`.
    //
    // After surgery, twins are re-matched by position so the bridge merges
    // cleanly with both the moved face and the unchanged adjacent geometry.
    //
    // None of these primitives mutate the host primitive's MeshData buffers —
    // call EditMesh.toMeshData(em) at the end to materialize.

    // Iterate the half-edges of a face: yields he, he.next, he.next.next.
    function faceHalfEdges(face) {
        const a = face.halfEdge;
        return [a, a.next, a.next.next];
    }

    // True when `he` is a face-group boundary: twin is null OR twin lives in a
    // different group than `he`.
    function isGroupBoundary(he) {
        if (!he.twin) return true;
        return he.twin.face.group !== he.face.group;
    }

    // Return the boundary loops of the face group `gIdx` as an array of
    // arrays. Each inner array is a list of HEs in order around the loop:
    // hes[i].next.origin and hes[i+1].origin sit at the same world position
    // (they may be DIFFERENT vertex objects when the mesh has seam-duplicate
    // verts — bromesh's cylinder cap puts rim[0] and rim[N] at the same
    // position but distinct verts to keep UVs sane).
    //
    // To advance from the current boundary HE to the next, we walk the
    // half-edge ring around the destination vertex via cur.next.twin.next
    // until we land on another boundary HE. This works across seam duplicates
    // because position-based twin-matching has already linked them.
    function findFaceGroupBoundary(em, gIdx) {
        const boundaryHEs = [];
        for (const he of em.halfEdges) {
            if (he.face.group !== gIdx) continue;
            if (isGroupBoundary(he)) boundaryHEs.push(he);
        }
        const visited = new Set();
        const loops = [];

        function nextBoundaryHE(he) {
            // Walk one-ring around he.next.origin in the same face group,
            // returning the next outgoing boundary HE.
            let cur = he.next;
            // Hard cap to detect malformed ring (open boundary on a fan vert
            // would loop forever otherwise).
            for (let i = 0; i < em.halfEdges.length + 1; i++) {
                if (cur.face.group === gIdx && isGroupBoundary(cur)) return cur;
                if (!cur.twin) return null;
                cur = cur.twin.next;
            }
            return null;
        }

        for (const start of boundaryHEs) {
            if (visited.has(start)) continue;
            const loop = [];
            let cur = start;
            while (cur && !visited.has(cur)) {
                visited.add(cur);
                loop.push(cur);
                cur = nextBoundaryHE(cur);
            }
            loops.push(loop);
        }
        return loops;
    }

    // Duplicate the verts on the face group's boundary at `vert + offset`,
    // and rewire the group's existing triangles to reference the duplicates.
    // Returns { dupMap, oldBoundary } where dupMap is a Map<oldVert, newVert>
    // and oldBoundary is an array of loops; each loop is an array of records
    //   { he, oldA, oldB, newA, newB, adjGroup }
    // capturing both the original endpoint verts (oldA, oldB) and their
    // duplicates (newA, newB). adjGroup is the group of the boundary HE's
    // twin face BEFORE severing (-1 if no twin). The HE's `.origin` after
    // this call points at newA — addBridge needs oldA/oldB explicitly
    // because they're no longer recoverable from the HE.
    //
    // INTERIOR verts of the face group (those not on the boundary — e.g. the
    // center vert of a fan-triangulated cylinder cap) are TRANSLATED in
    // place rather than duplicated. They have no other group referencing
    // them (otherwise they'd be on a boundary by definition), so moving
    // them outright keeps the face flat without breaking adjacency.
    //
    // Side effects:
    //   - new vertices appended to em.vertices at displaced boundary positions
    //   - interior verts mutated in place (x/y/z += offset)
    //   - each interior HE in the face group gets its origin updated to the
    //     duplicate (boundary verts) or stays pointing at the (now moved)
    //     interior vert
    //   - each boundary HE's twin (if any) is severed (twin set to null on
    //     both sides) — bridges will fill that gap
    function duplicateBoundary(em, gIdx, offset) {
        const loops = findFaceGroupBoundary(em, gIdx);
        if (loops.length === 0) {
            throw new Error('duplicateBoundary: face group ' + gIdx + ' has no boundary');
        }
        // Snapshot loops with original verts AND adjacent group before any
        // rewiring or severing. After severing, he.twin is null and the
        // adjacent group info is lost.
        const annotated = loops.map(loop => loop.map(he => ({
            he,
            oldA: he.origin,
            oldB: he.next.origin,
            newA: null,
            newB: null,
            adjGroup: he.twin ? he.twin.face.group : -1,
        })));
        const dupMap = new Map();
        for (const loop of annotated) {
            for (const rec of loop) {
                for (const v of [rec.oldA, rec.oldB]) {
                    if (dupMap.has(v)) continue;
                    const dup = {
                        x: v.x + offset[0],
                        y: v.y + offset[1],
                        z: v.z + offset[2],
                        halfEdge: null,
                    };
                    em.vertices.push(dup);
                    dupMap.set(v, dup);
                }
                rec.newA = dupMap.get(rec.oldA);
                rec.newB = dupMap.get(rec.oldB);
            }
        }
        // Walk the face group's verts. Boundary verts → rewire to dup.
        // Interior verts (not in dupMap) → translate in place once each.
        const interiorMoved = new Set();
        for (const he of em.halfEdges) {
            if (he.face.group !== gIdx) continue;
            const v = he.origin;
            const dup = dupMap.get(v);
            if (dup) {
                he.origin = dup;
                if (!dup.halfEdge) dup.halfEdge = he;
            } else if (!interiorMoved.has(v)) {
                v.x += offset[0];
                v.y += offset[1];
                v.z += offset[2];
                interiorMoved.add(v);
            }
        }
        // Original boundary verts may have lost their outgoing HE if it was
        // only referenced by this group. Refresh from any remaining HE.
        for (const v of dupMap.keys()) {
            if (v.halfEdge && v.halfEdge.origin === v) continue;
            v.halfEdge = null;
            for (const he of em.halfEdges) {
                if (he.origin === v) { v.halfEdge = he; break; }
            }
        }
        // Sever twins along the boundary — the duplicated face is now an
        // island floating one offset away. addBridge will reconnect.
        for (const loop of annotated) {
            for (const rec of loop) {
                if (rec.he.twin) {
                    rec.he.twin.twin = null;
                    rec.he.twin = null;
                }
            }
        }
        return { dupMap, oldBoundary: annotated };
    }

    // Add 2 triangles bridging the boundary edge oldA → oldB (captured before
    // duplicateBoundary rewired it) to its duplicates newA, newB. Quad layout:
    //   old_a → old_b → new_b → new_a
    // Triangles emitted (CCW seen from the bridge's outward side):
    //   T1: old_a, old_b, new_b
    //   T2: old_a, new_b, new_a
    // Caller-supplied `bridgeGroup` is set on both triangles' faces.
    //
    // Returns the 2 created faces.
    function addBridge(em, oldA, oldB, newA, newB, bridgeGroup) {
        if (!oldA || !oldB || !newA || !newB) {
            throw new Error('addBridge: missing endpoint vert');
        }
        const f1 = { halfEdge: null, group: bridgeGroup };
        const f2 = { halfEdge: null, group: bridgeGroup };
        em.faces.push(f1, f2);
        function makeHE(origin, face) {
            const he = { origin, twin: null, next: null, face };
            em.halfEdges.push(he);
            return he;
        }
        // Triangle 1: oldA → oldB → newB
        const h1a = makeHE(oldA, f1);
        const h1b = makeHE(oldB, f1);
        const h1c = makeHE(newB, f1);
        h1a.next = h1b; h1b.next = h1c; h1c.next = h1a;
        f1.halfEdge = h1a;
        // Triangle 2: oldA → newB → newA
        const h2a = makeHE(oldA, f2);
        const h2b = makeHE(newB, f2);
        const h2c = makeHE(newA, f2);
        h2a.next = h2b; h2b.next = h2c; h2c.next = h2a;
        f2.halfEdge = h2a;
        // Internal twin between T1 and T2 along their shared diagonal
        // (oldA → newB on T1 paired with newB → oldA on T2).
        h1c.twin = h2a; h2a.twin = h1c;
        // Refresh vertex outgoing-HE pointers if any of these verts had none.
        if (!oldA.halfEdge) oldA.halfEdge = h1a;
        if (!oldB.halfEdge) oldB.halfEdge = h1b;
        if (!newA.halfEdge) newA.halfEdge = h2c;
        if (!newB.halfEdge) newB.halfEdge = h1c;
        return [f1, f2];
    }

    // After all surgery, walk every HE pair without a twin and try to pair by
    // (origin, next.origin) vertex identity (within this EditMesh — same vert
    // object, not just position-equal). Twins span face groups, so this fixes
    // up the bridges' connections to existing geometry.
    function rematchTwins(em) {
        const byPair = new Map();
        for (const he of em.halfEdges) {
            if (he.twin) continue;
            byPair.set(he.origin + '>' + he.next.origin, he);
        }
        // Use a position-stable key since two distinct vertex *objects* may
        // sit at the same world position (cap rim duplicate of a wall vert).
        // Fall back to position-based matching if vertex-identity didn't pair.
        function pkey(v) {
            return Math.round(v.x * 1e5) + ',' +
                   Math.round(v.y * 1e5) + ',' +
                   Math.round(v.z * 1e5);
        }
        const byPos = new Map();
        for (const he of em.halfEdges) {
            if (he.twin) continue;
            const k = pkey(he.origin) + '|' + pkey(he.next.origin);
            if (!byPos.has(k)) byPos.set(k, he);
        }
        for (const he of em.halfEdges) {
            if (he.twin) continue;
            const k = pkey(he.next.origin) + '|' + pkey(he.origin);
            const cand = byPos.get(k);
            if (cand && cand !== he && !cand.twin) {
                he.twin = cand;
                cand.twin = he;
            }
        }
    }

    // toMeshData with face-group output. Returns the standard
    // {positions, indices} plus a Uint32Array `triToGroup` aligned to the
    // emitted triangle order. Verts that are unreferenced after surgery
    // (e.g. the originals when *all* their HEs were rewired to duplicates)
    // are dropped + remapped on the fly to keep the buffer compact.
    function toMeshDataWithGroups(em) {
        const usedVerts = new Set();
        for (const f of em.faces) {
            const hes = faceHalfEdges(f);
            for (const he of hes) usedVerts.add(he.origin);
        }
        const order = [];
        const idxOf = new Map();
        for (const v of em.vertices) {
            if (!usedVerts.has(v)) continue;
            idxOf.set(v, order.length);
            order.push(v);
        }
        const positions = new Float32Array(order.length * 3);
        for (let i = 0; i < order.length; i++) {
            positions[i*3 + 0] = order[i].x;
            positions[i*3 + 1] = order[i].y;
            positions[i*3 + 2] = order[i].z;
        }
        const indices = new Uint32Array(em.faces.length * 3);
        const triToGroup = new Int32Array(em.faces.length);
        for (let i = 0; i < em.faces.length; i++) {
            const hes = faceHalfEdges(em.faces[i]);
            indices[i*3 + 0] = idxOf.get(hes[0].origin);
            indices[i*3 + 1] = idxOf.get(hes[1].origin);
            indices[i*3 + 2] = idxOf.get(hes[2].origin);
            triToGroup[i] = em.faces[i].group;
        }
        return { positions, indices, triToGroup };
    }

    // --- Export -------------------------------------------------------------

    global.EditMesh = {
        fromMeshData,
        toMeshData,
        toMeshDataWithGroups,
        validate,
        // Surgery primitives.
        findFaceGroupBoundary,
        duplicateBoundary,
        addBridge,
        rematchTwins,
        isGroupBoundary,
        faceHalfEdges,
        // Exposed for tests; kept stable-ish since callers may want to hash
        // positions the same way we do.
        _posKey: posKey,
    };

})(typeof globalThis !== 'undefined' ? globalThis : this);
