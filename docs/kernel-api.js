/**
 * @file docs/kernel-api.js
 * @summary Documentation and examples for the `bro.kernel` JavaScript API.
 *
 * `bro.kernel` compiles brokernel source (../brokernel, a WGSL-like kernel
 * language for world code: f64 positions, grids, stencils and typed stores
 * as constructs, FastNoise2's nodes as `std::noise`) and runs its kernels on
 * the CPU: a point kernel per call (a height for physics, the camera,
 * placement), a grid kernel over a grid of cells into typed arrays. One
 * source compiles for the CPU and the GPU together; they compute the same
 * values (the language's rules, brokernel README.md).
 *
 * Mounted in every realm (the page's and a Worker's) when `BRO_WITH_KERNEL`
 * is enabled (the app, desktop and full profiles); a build without it has
 * `bro.kernel.available === false`.
 */

console.log('brokernel', bro.kernel.version);

// ============================================================================
// 1. Compiling: bro.kernel.compile(source, { file, modules }) -> KernelModule
// ============================================================================
//
// Every kernel of the source is compiled, synchronously. Nothing is read from
// disk: an import resolves against the standard library (`std::math`,
// `std::noise`, `std::filter`, `std::sphere`, `std::color`) and the `modules`
// given, a map of module name to source. `file` names the source in
// diagnostics (its directory, if any, is dropped).
//
// A compile error throws an Error whose message is the formatted diagnostics
// and whose `diagnostics` is [{ severity, file, line, column, message }].

const mod = bro.kernel.compile(`
    import std::noise::*;
    from world::shape import ridge;

    // A point kernel: no grid; called once per position.
    @kernel
    fn k_height(x: f64, y: f64, z: f64, seed: i32) -> f32 {
        let p = vec3<f64>(x, y, z) * 0.0005;
        return ridge(fbm_simplex<f64, 3>(p, 6, 2.0, 0.5, 0.0, seed) as f32);
    }

    // A grid kernel: one invocation per cell of a 2D grid.
    @kernel @grid(2)
    fn k_smooth(g: grid<2>, src: stencil<f32, 2>, out: write_store<f32, 2>) {
        let x = g.x();
        let y = g.y();
        out[x, y] = (src[-1, 0] + src[1, 0] + src[0, -1] + src[0, 1]) * 0.25;
    }

    // A grid kernel returning a value per cell.
    @kernel @grid(1)
    fn k_lookup(g: grid<1>, table: read_store<f32, 1>, idx: read_store<i32, 1>) -> f32 {
        return table[idx[g.x()]];
    }
`, {
    file: 'terrain.bk',
    modules: {
        'world::shape': `
            module world::shape;
            pub fn ridge(v: f32) -> f32 { return 1.0 - abs(v); }
        `,
    },
});

mod.kernels;                    // ['k_height', 'k_smooth', 'k_lookup']
const height = mod.kernel('k_height');   // a Kernel; throws for an unknown name

// ============================================================================
// 2. A Kernel's description
// ============================================================================

height.name;        // 'k_height'
height.gridDim;     // 0 for a point kernel, 1 to 3 for a grid kernel
height.returns;     // 'f32', or null for a kernel that returns nothing
height.cpuLanes;    // cells a grid kernel's CPU build computes at once (8 or 4 with AVX2, else 1)
height.params;      // what a call passes, in order (the grid parameter is not passed):
// [{ name: 'x', kind: 'scalar', type: 'f64' }, ..., { name: 'seed', kind: 'scalar', type: 'i32' }]
// A store's entry also has `access` ('read' | 'write' | 'read_write'), `dimension`,
// and `cells`: the grid axes the kernel accesses it at (below), 0 for none.

// ============================================================================
// 3. kernel.eval(...args): a point kernel at one point, on the CPU
// ============================================================================
//
// Arguments are positional, one per parameter: a number for a scalar (a
// BigInt also works for i64 / u64), an array of numbers for a vector
// (or a typed array of its bytes), a typed array of its bytes for a struct.
// The return is a number (a bool for bool), or an array of numbers for a
// vector or struct return.

const h = height.eval(2293560.0, 3058080.0, 5096800.0, 1337);

// ============================================================================
// 4. kernel.run(grid, ...args): a grid kernel over a grid, on the CPU
// ============================================================================
//
// `grid` is a width, or [width, height] / [width, height, depth] (no more
// axes than the kernel's grid). A store argument is a typed array of the
// store's element type (Float32Array for f32, Float64Array for f64,
// Int32Array for i32 and bool, Uint32Array for u32, BigInt64Array /
// BigUint64Array for i64 / u64; any typed array, read as bytes, for a vector
// or struct element). A store is shaped like the grid: [x, y] is element
// y * width + x. Stores are read and written in place; a kernel that returns
// a value returns a new typed array with one value per cell (one per leaf
// for a vector return, a Uint8Array of bytes for a mixed struct).

const W = 256, H = 256;
const field = new Float32Array(W * H);
const smoothed = new Float32Array(W * H);
mod.kernel('k_smooth').run([W, H], field, smoothed);

const values = mod.kernel('k_lookup').run(4, new Float32Array([5, 6, 7]), new Int32Array([2, 0, 9, -1]));
// Float32Array [7, 5, 0, 0]

// ============================================================================
// 5. Stores never leave their bounds
// ============================================================================
//
// A kernel cannot read or write outside the typed arrays it is given:
// - An access at the grid's cells (a stencil read, or an index of the cell's
//   own coordinates: g.x(), g.y(), g.z() in their places, g.linear_index()
//   into a 1D store) needs the store to hold the grid's cells over those axes
//   (the param's `cells`); a smaller one makes run() throw, naming it.
// - Any other index is bounded where it is used: outside the store a read is
//   zero and a write does nothing (k_lookup's 9 and -1 above).
//
// Errors: a wrong argument count, a non-number for a scalar or a typed array
// of the wrong element type for a store throws a TypeError; eval() of a grid
// kernel or run() of a point kernel throws a TypeError naming the other; a
// store too small for the grid throws an Error.
