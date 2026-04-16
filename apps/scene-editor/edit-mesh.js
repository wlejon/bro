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

    function fromMeshData(positions, indices) {
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
            const face = { halfEdge: null };
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

    // --- Export -------------------------------------------------------------

    global.EditMesh = {
        fromMeshData,
        toMeshData,
        validate,
        // Exposed for tests; kept stable-ish since callers may want to hash
        // positions the same way we do.
        _posKey: posKey,
    };

})(typeof globalThis !== 'undefined' ? globalThis : this);
