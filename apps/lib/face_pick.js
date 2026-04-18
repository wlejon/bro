// face_pick.js — N-gon-aware picking for editor / world-edit apps.
//
// Editors that store geometry as bromesh.PolyMesh (N-gon faces) need to
// translate a triangle-buffer raycast hit back into the logical face the
// user actually clicked on. This lib is the small wrapper around that.
//
// The "interaction mesh" pattern: a primitive holds both
//   - a render-side MeshData (positions/indices triangles + BVH for raycasts)
//   - an edit-side PolyMesh whose .tessellate() produced the render data
// Tessellation outputs `triToFace` aligned with the render index buffer;
// this lib uses that map to resolve a tri index to its source N-gon face.
//
// Generic over how primitives are stored: pass an iterable of
//   { id, mesh, bvh, polyMesh, triToFace, transformInvOrigin?, transformInvDir? }
// where transformInv* (optional) lets the lib raycast world-space rays
// against object-space BVHs. Most editor apps bake transforms into the
// mesh and can omit the transform.
//
// Usage:
//   const pick = FacePick.pickRay(ray, primitives);
//   if (pick) {
//       const face = pick.face;            // N-gon face index in PolyMesh
//       const tri  = pick.triangleIndex;   // triangle in render mesh
//       const verts = pick.faceVertices;   // ordered N-gon corner indices
//   }

(function (global) {
    'use strict';

    // Pick the nearest primitive whose BVH the ray hits. Returns:
    //   { primitive, hit, triangleIndex, position, normal, distance,
    //     face, group, faceVertices }
    // or null if no primitive is hit.
    //
    // `primitives` is any iterable of objects shaped as described above.
    // `opts.excludeId` (optional) skips one primitive (e.g. so Move can
    // see geometry behind the moving object).
    function pickRay(ray, primitives, opts) {
        const excludeId = opts && opts.excludeId != null ? opts.excludeId : null;
        let best = null;

        for (const prim of primitives) {
            if (!prim) continue;
            if (excludeId !== null && prim.id === excludeId) continue;
            if (!prim.bvh || !prim.mesh) continue;

            const hit = prim.bvh.raycast(prim.mesh, ray.origin, ray.dir, 0);
            if (!hit) continue;
            if (!best || hit.distance < best.hit.distance) {
                best = { primitive: prim, hit };
            }
        }
        if (!best) return null;
        return resolveHit(best.primitive, best.hit);
    }

    // Same as pickRay but takes only one primitive — useful when the caller
    // already knows which object was hit and just wants face resolution.
    function pickOnPrimitive(ray, prim) {
        if (!prim || !prim.bvh || !prim.mesh) return null;
        const hit = prim.bvh.raycast(prim.mesh, ray.origin, ray.dir, 0);
        if (!hit) return null;
        return resolveHit(prim, hit);
    }

    // Given a raw tri-buffer hit, attach face/group/faceVertices info.
    // Falls back gracefully when the primitive has no PolyMesh attached.
    function resolveHit(prim, hit) {
        const out = {
            primitive: prim,
            hit,
            triangleIndex: hit.triangleIndex,
            position:      hit.position,
            normal:        hit.normal,
            distance:      hit.distance,
            face:           -1,
            group:          -1,
            faceVertices:   null,
        };
        // Primary path: tri-to-face map from the most recent tessellation.
        if (prim.triToFace && hit.triangleIndex < prim.triToFace.length) {
            out.face = prim.triToFace[hit.triangleIndex];
        }
        if (prim.triToGroup && hit.triangleIndex < prim.triToGroup.length) {
            out.group = prim.triToGroup[hit.triangleIndex];
        }
        if (prim.polyMesh && out.face >= 0) {
            out.faceVertices = prim.polyMesh.faceVertices(out.face);
            // If the primitive didn't supply triToGroup, fall back to the
            // PolyMesh's face.group.
            if (out.group < 0) out.group = prim.polyMesh.faceGroup(out.face);
        }
        return out;
    }

    // Build a triangle-to-face lookup by tessellating a PolyMesh and
    // returning both the render MeshData and the maps. This is the typical
    // primitive-construction helper:
    //
    //   const r = FacePick.tessellateForRender(polyMesh);
    //   primitive.mesh        = new Mesh({positions: r.positions, indices: r.indices,
    //                                     normals: r.normals});
    //   primitive.triToFace   = r.triToFace;
    //   primitive.triToGroup  = r.triToGroup;
    function tessellateForRender(polyMesh) {
        const t = polyMesh.tessellate();
        return {
            positions:  t.positions,
            normals:    t.normals,
            indices:    t.indices,
            triToFace:  t.triToFace,
            triToGroup: t.triToGroup,
        };
    }

    global.FacePick = {
        pickRay,
        pickOnPrimitive,
        resolveHit,
        tessellateForRender,
    };

})(typeof globalThis !== 'undefined' ? globalThis : this);
