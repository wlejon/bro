// Verify bro.mesh.PolyMesh tessellation against mesh-rendering-bug.bro.
//
// Loads Polygon 2 from the saved project, builds a PolyMesh that *collapses*
// the existing tris into N-gon faces by group, retessellates, and confirms
// no triangle edge crosses a polygon boundary edge.
'use strict';

advanceTime(0); flush();

// `Mesh` is bro.mesh.Mesh; PolyMesh is its sibling under the same surface.
// In the binding, both are registered as top-level constructors via qjsbind.
const PolyMesh = globalThis.PolyMesh;

assert(typeof PolyMesh === 'function',
    'PolyMesh constructor must be exposed globally (got ' + typeof PolyMesh + ')');

// --- 1. Smoke: empty constructor + tessellate of empty -------------------
{
    const pm = new PolyMesh();
    assert(pm.faceCount === 0, 'empty pm has 0 faces');
    const t = pm.tessellate();
    assert(t.indices.length === 0, 'empty tessellation');
    console.log('  ok smoke: empty PolyMesh');
}

// --- 2. Single triangle round-trip --------------------------------------
{
    const positions = new Float32Array([0,0,0,  1,0,0,  0,1,0]);
    const indices   = new Uint32Array([0,1,2]);
    const pm = PolyMesh.fromMeshData(positions, indices);
    assert(pm.faceCount === 1, '1 face from 1 tri');
    assert(pm.faceVertexCount(0) === 3, 'face is a triangle');
    const v = pm.validate();
    assert(v.valid, 'validate ok: ' + v.errors.join(';'));
    assert(!v.isClosed, 'one tri is open');
    assert(v.boundaryHalfEdges === 3, '3 boundary HEs');
    const t = pm.tessellate();
    assert(t.indices.length === 3, 'one tri out');
    console.log('  ok smoke: single triangle');
}

// --- 3. Bug file: collapse fan into N-gon, retessellate, count crossings -
{
    const proj = JSON.parse(require('fs').readFileSync(
        'D:/bro-test/mesh-rendering-bug.bro/project.json', 'utf8'));
    const poly = proj.data.primitives.find(p => p.name === 'Polygon 2');
    const P = new Float32Array(poly.positions);
    const I = new Uint32Array(poly.indices);

    // Use coplanar grouping: tag tris so the top cap (4 tris) becomes one
    // group and the bottom cap becomes another. We can detect via Y-coord
    // (top: y≈4.07, bottom: y≈0). Side walls each get their own group.
    const triCount = I.length / 3;
    const triToGroup = new Uint32Array(triCount);
    const SIDE_BASE = 100;   // arbitrary IDs for sides
    let nextSideGroup = SIDE_BASE;
    const sideGroupByEdgeKey = new Map();
    const Q = 1e3;
    function qkey(vi) {
        return Math.round(P[vi*3]*Q) + ',' + Math.round(P[vi*3+1]*Q) + ',' +
               Math.round(P[vi*3+2]*Q);
    }
    for (let t = 0; t < triCount; t++) {
        const ys = [P[I[t*3]*3+1], P[I[t*3+1]*3+1], P[I[t*3+2]*3+1]];
        const allTop    = ys.every(y => y > 3);
        const allBottom = ys.every(y => Math.abs(y) < 0.01);
        if      (allTop)    triToGroup[t] = 1;
        else if (allBottom) triToGroup[t] = 2;
        else {
            // Side wall: tris on the same wall share the same vertical edge
            // (the boundary edge between the two y-layers). Find that edge
            // (the one with two distinct y values along it where both ends
            // are at y=0 OR both ends are at y=top — wait, vertical edges go
            // y=0 to y=top). Pick the pair of vertices at y=0; their position
            // pair is unique per wall.
            const at0 = [];
            for (let k = 0; k < 3; k++) {
                const vi = I[t*3+k];
                if (Math.abs(P[vi*3+1]) < 0.01) at0.push(vi);
            }
            // Two of the 3 verts should be at y=0 for a side wall tri.
            at0.sort((a,b) => qkey(a) < qkey(b) ? -1 : 1);
            const key = at0.length >= 2 ? (qkey(at0[0]) + '|' + qkey(at0[1])) :
                                          ('only1@' + qkey(at0[0] || 0));
            if (!sideGroupByEdgeKey.has(key)) {
                sideGroupByEdgeKey.set(key, nextSideGroup++);
            }
            triToGroup[t] = sideGroupByEdgeKey.get(key);
        }
    }

    const pm = PolyMesh.fromMeshData(P, I, triToGroup);
    console.log(`  bug-file PolyMesh (raw):    faces=${pm.faceCount} verts=${pm.vertexCount}`);
    pm.mergeFacesByGroup();
    console.log(`  bug-file PolyMesh (merged): faces=${pm.faceCount} verts=${pm.vertexCount}`);
    // Expect: 1 top cap face (with 6 verts), 1 bottom cap face (6 verts),
    // 6 side wall faces (4 verts each).  Plus possibly 1 face per stray tri.
    const v = pm.validate();
    console.log(`  validate: valid=${v.valid} closed=${v.isClosed} boundary=${v.boundaryHalfEdges}`);

    // Check N-gon counts: top cap should be a 6-gon (hexagon).
    let nGonHistogram = new Map();
    for (let f = 0; f < pm.faceCount; f++) {
        const n = pm.faceVertexCount(f);
        nGonHistogram.set(n, (nGonHistogram.get(n) || 0) + 1);
    }
    console.log('  N-gon counts:', JSON.stringify(Array.from(nGonHistogram.entries())));

    // Tessellate and check NO triangle edge crosses a polygon boundary edge.
    const tess = pm.tessellate();
    console.log(`  tessellated: tris=${tess.indices.length/3} verts=${tess.positions.length/3}`);

    // Detect bad triangulation: re-detect T-junctions / boundary crossings.
    const TP = tess.positions, TI = tess.indices;
    const QUANT = 1e5;
    function tqkey(x,y,z){ return Math.round(x*QUANT)+','+Math.round(y*QUANT)+','+Math.round(z*QUANT); }
    function pointOnSegment(A,B,p,eps){
        const abx=B[0]-A[0],aby=B[1]-A[1],abz=B[2]-A[2];
        const apx=p[0]-A[0],apy=p[1]-A[1],apz=p[2]-A[2];
        const L2=abx*abx+aby*aby+abz*abz; if(L2<1e-18) return false;
        const t=(apx*abx+apy*aby+apz*abz)/L2; if(t<=eps||t>=1-eps) return false;
        const cx=A[0]+t*abx,cy=A[1]+t*aby,cz=A[2]+t*abz;
        return ((p[0]-cx)**2 + (p[1]-cy)**2 + (p[2]-cz)**2) < eps*eps;
    }
    // Build unique positions across the tessellated mesh.
    const byKey=new Map(); const uPos=[];
    for (let i = 0; i < TP.length/3; i++){
        const k=tqkey(TP[i*3],TP[i*3+1],TP[i*3+2]);
        if (!byKey.has(k)) { byKey.set(k, uPos.length); uPos.push([TP[i*3],TP[i*3+1],TP[i*3+2]]); }
    }
    let tjunctions = 0;
    const seenEdge = new Set();
    for (let t = 0; t < TI.length/3; t++) {
        const a=TI[t*3],b=TI[t*3+1],c=TI[t*3+2];
        const tri=[a,b,c].map(i=>byKey.get(tqkey(TP[i*3],TP[i*3+1],TP[i*3+2])));
        for (let k = 0; k < 3; k++) {
            const x=tri[k], y=tri[(k+1)%3]; if(x===y) continue;
            const ek = x<y ? (x+'_'+y) : (y+'_'+x);
            if (seenEdge.has(ek)) continue; seenEdge.add(ek);
            const A=uPos[x], B=uPos[y];
            for (let z = 0; z < uPos.length; z++) {
                if (z===x||z===y) continue;
                if (pointOnSegment(A,B,uPos[z],1e-3)) {
                    tjunctions++;
                    if (tjunctions <= 5) {
                        console.log(`    T-junction: edge [${A.map(n=>n.toFixed(3))}]→[${B.map(n=>n.toFixed(3))}] has interior vert [${uPos[z].map(n=>n.toFixed(3))}]`);
                    }
                }
            }
        }
    }
    console.log(`  T-junctions in retessellation: ${tjunctions}`);

    // Also: no tessellated triangle edge should cross a polygon-face boundary edge.
    // For each face, collect its boundary edges and verify no other triangle edge
    // intersects them.
    let crossings = 0;
    function segCross2D(A,B,C,D, axisU, axisV) {
        const a = [A[0]*axisU[0]+A[1]*axisU[1]+A[2]*axisU[2],
                   A[0]*axisV[0]+A[1]*axisV[1]+A[2]*axisV[2]];
        const b = [B[0]*axisU[0]+B[1]*axisU[1]+B[2]*axisU[2],
                   B[0]*axisV[0]+B[1]*axisV[1]+B[2]*axisV[2]];
        const c = [C[0]*axisU[0]+C[1]*axisU[1]+C[2]*axisU[2],
                   C[0]*axisV[0]+C[1]*axisV[1]+C[2]*axisV[2]];
        const d = [D[0]*axisU[0]+D[1]*axisU[1]+D[2]*axisU[2],
                   D[0]*axisV[0]+D[1]*axisV[1]+D[2]*axisV[2]];
        const r=[b[0]-a[0],b[1]-a[1]], s=[d[0]-c[0],d[1]-c[1]];
        const den=r[0]*s[1]-r[1]*s[0]; if(Math.abs(den)<1e-12) return false;
        const dx=c[0]-a[0],dy=c[1]-a[1];
        const tt=(dx*s[1]-dy*s[0])/den, uu=(dx*r[1]-dy*r[0])/den;
        const E=1e-4;
        return tt>E && tt<1-E && uu>E && uu<1-E;
    }
    for (let f = 0; f < pm.faceCount; f++) {
        const verts = pm.faceVertices(f);
        if (verts.length < 4) continue;     // triangles can't self-cross
        const n = pm.computeFaceNormal(f);
        const ax=Math.abs(n[0]),ay=Math.abs(n[1]),az=Math.abs(n[2]);
        let tmp=[0,0,0]; if(ax<=ay&&ax<=az) tmp=[1,0,0]; else if(ay<=ax&&ay<=az) tmp=[0,1,0]; else tmp=[0,0,1];
        const u=[tmp[1]*n[2]-tmp[2]*n[1],tmp[2]*n[0]-tmp[0]*n[2],tmp[0]*n[1]-tmp[1]*n[0]];
        const Lu=Math.hypot(u[0],u[1],u[2]); u[0]/=Lu;u[1]/=Lu;u[2]/=Lu;
        const v2=[n[1]*u[2]-n[2]*u[1],n[2]*u[0]-n[0]*u[2],n[0]*u[1]-n[1]*u[0]];

        // Polygon edges (consecutive verts).
        const edges = [];
        for (let i = 0; i < verts.length; i++) {
            const a = verts[i], b = verts[(i+1)%verts.length];
            edges.push([pm.getVertex(a), pm.getVertex(b), a, b]);
        }
        // Triangle edges from tessellation belonging to this face.
        for (let t = 0; t < tess.triToFace.length; t++) {
            if (tess.triToFace[t] !== f) continue;
            const ti = [TI[t*3], TI[t*3+1], TI[t*3+2]];
            const tp = ti.map(i => [TP[i*3],TP[i*3+1],TP[i*3+2]]);
            for (let k = 0; k < 3; k++) {
                const A = tp[k], B = tp[(k+1)%3];
                for (const [C, D] of edges) {
                    if (segCross2D(A,B,C,D,u,v2)) crossings++;
                }
            }
        }
    }
    console.log(`  Triangle edges crossing polygon boundary edges: ${crossings}`);

    if (tjunctions === 0 && crossings === 0) {
        console.log('  *** PASS: PolyMesh.tessellate produces a clean triangulation ***');
    } else {
        console.log(`  *** FAIL: ${tjunctions} T-junctions, ${crossings} crossings ***`);
    }
}
