/**
 * @file docs/kernel-api.js
 * @summary Documentation and examples for the `bro.kernel` JavaScript API.
 *
 * `bro.kernel` compiles brokernel source (../brokernel, a WGSL-like kernel
 * language for world code: f64 positions, grids, stencils and typed stores
 * as constructs, FastNoise2's nodes as `std::noise`) and runs its kernels on
 * the CPU: a point kernel per call (a height for physics, the camera,
 * placement), a grid kernel over a grid of cells into typed arrays; and on
 * the GPU: a grid kernel dispatched on the engine's own Vulkan device over
 * KernelBuffers (section 6). One source compiles for the CPU and the GPU
 * together; they compute the same values (the language's rules, brokernel
 * README.md).
 *
 * Mounted in every realm (the page's and a Worker's) when `BRO_WITH_KERNEL`
 * is enabled (the app, desktop and full profiles); a build without it has
 * `bro.kernel.available === false`. The GPU half is the main thread's only:
 * a Worker has `bro.kernel.gpu === false`.
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

// ============================================================================
// 6. The GPU: bro.kernel.createBuffer() and kernel.dispatch(grid, ...args)
// ============================================================================
//
// `bro.kernel.gpu` is true once the engine's Vulkan device is up (it can
// reach buffers by address and compute in 64-bit integers; f64 kernels also
// need shaderFloat64). The work runs in the engine's graphics stream, in the
// order it is asked for, before any later frame's draws.
//
// bro.kernel.createBuffer(byteLength | typedArray) -> KernelBuffer: GPU
// memory, reading zero, or holding the typed array's bytes. A KernelBuffer is
// untyped bytes; a store's elements are its byteLength / the element size.
//   buffer.byteLength
//   buffer.write(typedArray, byteOffset = 0)   // copied now, lands before later dispatches
//   buffer.read(typedArray, byteOffset = 0)    // fills it after every dispatch so far
//                                              // (blocks until they are done); returns it
//
// kernel.dispatch(grid, ...args) takes run()'s arguments with a KernelBuffer
// for each store. A kernel that returns a value returns a new KernelBuffer of
// one value per cell, laid out as run()'s typed array; one that returns
// nothing returns undefined. Stores are bounded as on the CPU (section 5).
//
// A scene draws from a KernelBuffer in place, with no read back: a node's
// custom shader declares `buffer float u_heights[];` and the node binds the
// buffer with setShaderBuffer('u_heights', buffer) (scene-nodes-api.js).
// What is dispatched before a frame is what that frame draws.

if (bro.kernel.gpu) {
    const fieldBuf = bro.kernel.createBuffer(field);          // its bytes, on the GPU
    const smoothBuf = bro.kernel.createBuffer(W * H * 4);     // reads zero
    const smooth = mod.kernel('k_smooth');
    smooth.dispatch([W, H], fieldBuf, smoothBuf);
    smooth.dispatch([W, H], smoothBuf, fieldBuf);             // runs after the first
    fieldBuf.read(smoothed);                                  // both done

    const looked = mod.kernel('k_lookup').dispatch(4,
        bro.kernel.createBuffer(new Float32Array([5, 6, 7])),
        bro.kernel.createBuffer(new Int32Array([2, 0, 9, -1])));
    looked.read(new Float32Array(4));                         // [7, 5, 0, 0]
}

// Errors: a typed array where a KernelBuffer goes, dispatch() of a point
// kernel, or a read / write outside the buffer (RangeError) throws; a kernel
// the device cannot run (a capability it lacks) throws an Error naming it.

// ============================================================================
// 7. bro.kernel.background(fn) -> BackgroundJob: work beside the frames
// ============================================================================
//
// fn() is called at once, and every dispatch, write and createBuffer it makes
// goes to the background stream (render::VulkanCompute::beginBackground):
// the GPU's async compute queue (in order on the graphics queue where the
// device has none), after the work recorded before it, in order with
// earlier background jobs, and beside the frames recorded after it. For
// work whose results are wanted frames later, not this frame: a world's
// field rebuilt into the half of a double buffer it does not draw from. A
// few small dependent dispatches are far from filling the GPU, so in the
// frame's own stream they lengthen the frame by all of their latency, where
// beside it they cost only what they slow its work by. (A tile cache's
// production runs here already, tiles-api.js.)
//
//   job.done   true once the GPU has done the job's work; from the read that
//              finds it true, the frames read what it wrote (the graphics
//              queue waits for it, a wait already passed)
//
// Until done, nothing may rewrite a buffer the job's kernels read (write it,
// or dispatch into it outside the job), nor read what they write: the GPU
// may still be at it a frame later. A buffer read() inside fn waits for the
// job's work so far. fn's throw is rethrown (what it recorded still runs); a
// job inside a job, and TileCache.update inside one, throw a TypeError.
// perf.kernelTimes lists its kernels as "<kernel> (background)".

if (bro.kernel.gpu) {
    const source = bro.kernel.createBuffer(field);
    const fields = [bro.kernel.createBuffer(W * H * 4), bro.kernel.createBuffer(W * H * 4)];
    let front = 0, job = null;
    // A frame: start a rebuild into the back half when wanted, and swap once
    // it is done; the scene draws fields[front] meanwhile.
    function frame(wanted) {
        if (!job && wanted) {
            const back = fields[1 - front];
            job = bro.kernel.background(() => mod.kernel('k_smooth').dispatch([W, H], source, back));
        }
        if (job && job.done) {
            front = 1 - front;   // node.setShaderBuffer('u_field', fields[front])
            job = null;
        }
    }
}
