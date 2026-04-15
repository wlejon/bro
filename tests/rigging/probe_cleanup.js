// Diagnose mesh-forge cleanup pipeline on the real MeshyAI Titan glb.
// Replicates stages/cleanup.js step-by-step and prints mesh stats at each
// stage so we can see which operation changes topology.

const path = 'D:/moba-game/init-Meshy_AI_T_Pose_Titan_0415091834_generate.glb';

function stats(label, m) {
    const V = m.vertexCount, T = m.triangleCount;
    const ratio = V > 0 ? (T / V).toFixed(3) : '-';
    console.log(label.padEnd(28)
        + ' verts=' + String(V).padStart(6)
        + ' tris='  + String(T).padStart(6)
        + ' T/V='   + ratio);
}

const g = Mesh.loadGLTF(path);
console.log('file: ' + path.split('/').pop());
console.log('meshes=' + g.meshes.length + ' skins=' + g.skins.length
    + ' skeletons=' + g.skeletons.length);

const src = g.meshes[0];
stats('source', src);

const bb = src.computeBBox();
const diag = Math.hypot(bb.max[0]-bb.min[0], bb.max[1]-bb.min[1], bb.max[2]-bb.min[2]);
console.log('bbox diag=' + diag.toFixed(4));
const edgeLen = diag / 200;
console.log('auto edgeLen=' + edgeLen.toFixed(5));

function cloneMesh(m) {
    return new Mesh({
        positions: new Float32Array(m.positions),
        indices:   new Uint32Array(m.indices),
        normals:   m.hasNormals ? new Float32Array(m.normals) : undefined,
        uvs:       m.hasUVs     ? new Float32Array(m.uvs) : undefined,
    });
}

function edgeAudit(m) {
    // Count edges by adjacent-face count, using position-welded canonical ids.
    const pos = m.positions, idx = m.indices;
    const V = m.vertexCount;
    const q = new Map();
    const canon = new Uint32Array(V);
    let nextId = 0;
    for (let v = 0; v < V; v++) {
        const k = Math.round(pos[v*3]*10000) + ',' +
                  Math.round(pos[v*3+1]*10000) + ',' +
                  Math.round(pos[v*3+2]*10000);
        let id = q.get(k);
        if (id === undefined) { id = nextId++; q.set(k, id); }
        canon[v] = id;
    }
    const edges = new Map();
    const T = idx.length / 3;
    for (let t = 0; t < T; t++) {
        for (let e = 0; e < 3; e++) {
            let a = canon[idx[t*3+e]], b = canon[idx[t*3+((e+1)%3)]];
            if (a > b) { const tmp = a; a = b; b = tmp; }
            const k = a + '_' + b;
            edges.set(k, (edges.get(k) || 0) + 1);
        }
    }
    let boundary = 0, interior = 0, nonMan = 0;
    for (const [, c] of edges) {
        if (c === 1) boundary++;
        else if (c === 2) interior++;
        else nonMan++;
    }
    return { total: edges.size, boundary, interior, nonMan };
}

function windingAudit(m) {
    // Each shared (undirected) edge should be traversed in opposite
    // directions by the two incident faces. Count (a→b) occurrences
    // as +1 and (b→a) as -1 grouped by unordered edge; sum should be 0.
    const idx = m.indices, T = idx.length / 3;
    const dir = new Map();
    for (let t = 0; t < T; t++) {
        for (let e = 0; e < 3; e++) {
            const a = idx[t*3+e], b = idx[t*3+((e+1)%3)];
            const lo = Math.min(a,b), hi = Math.max(a,b);
            const k = lo + '_' + hi;
            const sign = (a === lo) ? 1 : -1;
            dir.set(k, (dir.get(k) || 0) + sign);
        }
    }
    let bad = 0;
    for (const [, s] of dir) if (s !== 0) bad++;
    return bad;
}

function check(label, m) {
    stats(label, m);
    const a = edgeAudit(m);
    const w = windingAudit(m);
    console.log('  manifold=' + m.isManifold()
        + ' edges=' + a.total
        + ' boundary(1)=' + a.boundary
        + ' interior(2)=' + a.interior
        + ' nonMan(3+)=' + a.nonMan
        + ' windingMismatches=' + w);
}

check('source', src);

// UI default pipeline (weld 2e-5, remesh auto iter=3, smoothTaubin 10).
{
    const m = cloneMesh(src);
    m.weld(2e-5);
    check('after weld', m);
    m.remeshIsotropic(edgeLen, 1);
    check('+ remesh iter=1', m);
}
{
    const m = cloneMesh(src);
    m.weld(2e-5);
    m.remeshIsotropic(edgeLen, 2);
    check('remesh iter=2', m);
}
{
    const m = cloneMesh(src);
    m.weld(2e-5);
    m.remeshIsotropic(edgeLen, 3);
    check('remesh iter=3', m);
    m.smoothTaubin(0.5, -0.53, 10);
    check('+ smoothTaubin(10)', m);
}

// Same pipeline without remesh — should be clean.
{
    const m = cloneMesh(src);
    m.weld(2e-5);
    m.smoothTaubin(0.5, -0.53, 10);
    check('weld+smooth only', m);
}
