// applyMorphTarget — adds delta positions/normals scaled by weight.

const m = Mesh.box(0.5, 0.5, 0.5);
const before = new Float32Array(m.positions);
const vc = m.vertexCount;

// Morph that translates every vertex by (1, 0, 0).
const dp = new Float32Array(vc * 3);
for (let i = 0; i < vc; i++) dp[i * 3] = 1.0;

m.applyMorphTarget({ name: 'shift', deltaPositions: dp }, 0.5);
const half = m.positions;
for (let i = 0; i < vc; i++) {
    assert(Math.abs(half[i*3] - (before[i*3] + 0.5)) < 1e-4,
        'half-weight morph at vertex ' + i + ': got ' + half[i*3] + ' want ' + (before[i*3] + 0.5));
    // y, z untouched
    assert(Math.abs(half[i*3+1] - before[i*3+1]) < 1e-4, 'y unchanged');
    assert(Math.abs(half[i*3+2] - before[i*3+2]) < 1e-4, 'z unchanged');
}

// Apply the same morph at full weight again; cumulative shift should be 1.5.
m.applyMorphTarget({ name: 'shift', deltaPositions: dp }, 1.0);
const full = m.positions;
for (let i = 0; i < vc; i++) {
    assert(Math.abs(full[i*3] - (before[i*3] + 1.5)) < 1e-4,
        'cumulative morph at vertex ' + i);
}

// Zero weight is a no-op.
const snap = new Float32Array(m.positions);
m.applyMorphTarget({ name: 'shift', deltaPositions: dp }, 0.0);
const post = m.positions;
for (let i = 0; i < snap.length; i++)
    assert(snap[i] === post[i], 'zero-weight morph is a no-op');

console.log('PASS test_morph');
