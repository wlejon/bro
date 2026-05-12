// Test FastNoise binding.

assert(typeof FastNoise === 'function', 'FastNoise constructor exists');
assert(typeof FastNoise.create === 'function', 'FastNoise.create fn');
assert(typeof FastNoise.Simplex === 'function', 'FastNoise.Simplex fn');

const simplex = FastNoise.Simplex();
assert(simplex !== null && typeof simplex === 'object', 'simplex created');
assert(typeof simplex.genSingle2D === 'function', 'genSingle2D fn');

// Sample a few points
const a = simplex.genSingle2D(1.5, 2.5, 1337);
const b = simplex.genSingle2D(10.5, 20.5, 1337);
assert(typeof a === 'number', 'sample is number: ' + typeof a);
assert(a >= -2 && a <= 2, 'sample in reasonable range: ' + a);
assert(typeof b === 'number', 'sample b is number');

// Determinism: same args → same result
const a2 = simplex.genSingle2D(1.5, 2.5, 1337);
assert(a === a2, 'deterministic: ' + a + ' vs ' + a2);

// Varies with position
assert(a !== b, 'varies with position');

// Different seed → different result (usually)
const c = simplex.genSingle2D(1.5, 2.5, 9999);
// Not guaranteed always different but very likely
assert(c !== a || c === 0, 'seed changes result');

// 3D
if (typeof simplex.genSingle3D === 'function') {
    const v3 = simplex.genSingle3D(1.5, 2.5, 3.5, 1337);
    assert(typeof v3 === 'number', '3D sample is number: ' + v3);
}

// Grid
if (typeof simplex.genUniformGrid2D === 'function') {
    const grid = simplex.genUniformGrid2D(0, 0, 8, 8, 0.1, 1337);
    assert(grid instanceof Float32Array, 'grid is Float32Array');
    assert(grid.length === 64, 'grid length 64: ' + grid.length);
    let any = false;
    for (let i = 0; i < grid.length; i++) if (grid[i] !== 0) { any = true; break; }
    assert(any, 'grid has nonzero');
}

// types()
if (typeof FastNoise.types === 'function') {
    const t = FastNoise.types();
    assert(Array.isArray(t), 'types is array');
    assert(t.length > 0, 'types nonempty: ' + t.length);
}

// FractalFBm composition
const fbm = FastNoise.create('FractalFBm');
assert(fbm !== null, 'FractalFBm created');
fbm.set('Source', FastNoise.Simplex());
fbm.set('Octaves', 3);
const fv = fbm.genSingle2D(1.2, 3.4, 42);
assert(typeof fv === 'number', 'fbm sample: ' + fv);
