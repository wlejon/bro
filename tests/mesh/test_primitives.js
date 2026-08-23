// Mesh primitives — parametric and procedural / Platonic solids.

function nonEmpty(m, name) {
    assert(!m.empty, name + ' non-empty');
    assert(m.vertexCount > 0, name + ' has vertices');
    assert(m.triangleCount > 0, name + ' has triangles');
}

// Existing parametric
nonEmpty(Mesh.box(),      'box');
nonEmpty(Mesh.sphere(),   'sphere');
nonEmpty(Mesh.cylinder(), 'cylinder');
nonEmpty(Mesh.capsule(),  'capsule');
nonEmpty(Mesh.plane(),    'plane');
nonEmpty(Mesh.torus(),    'torus');

// New par_primitives
nonEmpty(Mesh.geodesicSphere(1, 2), 'geodesicSphere');
nonEmpty(Mesh.icosahedron(),        'icosahedron');
nonEmpty(Mesh.dodecahedron(),       'dodecahedron');
nonEmpty(Mesh.octahedron(),         'octahedron');
nonEmpty(Mesh.tetrahedron(),        'tetrahedron');
nonEmpty(Mesh.cone(0.5, 1, 16, 4),  'cone');
nonEmpty(Mesh.disc(0.5, 16),        'disc');
nonEmpty(Mesh.rock(1.0, 7, 2),      'rock');

// Platonic solid face counts
const ico = Mesh.icosahedron();
assert(ico.triangleCount === 20, 'icosahedron has 20 faces, got ' + ico.triangleCount);
const octa = Mesh.octahedron();
assert(octa.triangleCount === 8, 'octahedron has 8 faces, got ' + octa.triangleCount);
const tet = Mesh.tetrahedron();
assert(tet.triangleCount === 4, 'tetrahedron has 4 faces, got ' + tet.triangleCount);

// geodesic sphere subdivision math: 20 * 4^n triangles
const g0 = Mesh.geodesicSphere(1, 0);
assert(g0.triangleCount === 20, 'geodesic n=0 = 20 tris');
const g1 = Mesh.geodesicSphere(1, 1);
assert(g1.triangleCount === 80, 'geodesic n=1 = 80 tris');
const g2 = Mesh.geodesicSphere(1, 2);
assert(g2.triangleCount === 320, 'geodesic n=2 = 320 tris');

// Heightmap grid
const h = new Float32Array(16);
for (let i = 0; i < 16; i++) h[i] = Math.sin(i);
const hm = Mesh.heightmapGrid(h, 4, 4, 1.0);
nonEmpty(hm, 'heightmapGrid');
