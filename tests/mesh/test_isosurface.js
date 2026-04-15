// Isosurface extraction: marching cubes, dual contouring, surface nets.

function sphereField(gx, gy, gz, radius) {
    const field = new Float32Array(gx * gy * gz);
    const cx = (gx - 1) / 2, cy = (gy - 1) / 2, cz = (gz - 1) / 2;
    for (let z = 0; z < gz; z++) {
        for (let y = 0; y < gy; y++) {
            for (let x = 0; x < gx; x++) {
                const dx = x - cx, dy = y - cy, dz = z - cz;
                field[(z * gy + y) * gx + x] = Math.sqrt(dx*dx + dy*dy + dz*dz) - radius;
            }
        }
    }
    return field;
}

const N = 16;
const f = sphereField(N, N, N, 5);

// Marching cubes
const mc = Mesh.marchingCubes(f, N, N, N, 0, 1);
assert(!mc.empty, 'marching cubes produces non-empty mesh');
assert(mc.triangleCount > 100, 'mc has lots of triangles for a sphere; got ' + mc.triangleCount);

// Dual contouring
const dc = Mesh.dualContour(f, N, N, N, 0, 1);
assert(!dc.empty, 'dual contour produces non-empty mesh');

// Surface nets (new)
const sn = Mesh.surfaceNets(f, N, N, N, 0, 1);
assert(!sn.empty, 'surface nets produces non-empty mesh');

// Transvoxel (new) — cubic grid only
const gridSize = 16;
const f2 = sphereField(gridSize, gridSize, gridSize, 5);
const tv = Mesh.transvoxel(f2, gridSize, 0, [-1, -1, -1, -1, -1, -1], 0, 1);
assert(!tv.empty, 'transvoxel produces non-empty mesh');

// Greedy mesh on a filled voxel cube
const gx = 8, gy = 8, gz = 8;
const voxels = new Uint8Array(gx * gy * gz);
for (let z = 2; z < 6; z++)
    for (let y = 2; y < 6; y++)
        for (let x = 2; x < 6; x++)
            voxels[(z * gy + y) * gx + x] = 1;

const gm = Mesh.greedyMesh(voxels, gx, gy, gz, 1);
assert(!gm.empty, 'greedy mesh non-empty');
assert(gm.triangleCount === 12, '4x4x4 box greedy-meshes to 12 triangles; got ' + gm.triangleCount);
