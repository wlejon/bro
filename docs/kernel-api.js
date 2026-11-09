/**
 * @file docs/kernel-api.js
 * @summary Documentation and examples for the `bro.kernel` JavaScript API.
 *
 * `bro.kernel` compiles brokernel source (../brokernel, a WGSL-like kernel
 * language for world code: grids, stencils and typed stores as constructs,
 * FastNoise2's nodes as `std::noise`) and runs its kernels on the CPU: a
 * point kernel per call (a height for physics, the camera, placement), a
 * grid kernel over a grid of cells into typed arrays; and on the GPU: a grid
 * kernel dispatched on the engine's own Vulkan device over KernelBuffers
 * (section 6), its pixels shown in the page by a 'kernel' canvas (section
 * 8). One source compiles for the CPU and the GPU together. brokernel is the
 * one way an app programs the GPU; WebGL2 is there for web content.
 *
 * GPU code is f32. f64 is the CPU's: a kernel marked `@cpu` has no GPU build
 * and may use it (absolute positions on an Earth-radius world, a noise
 * lattice's cell); every other kernel is compiled for the GPU too, and f64 in
 * one is a compile error at the f64. Work about a large world is done in f64
 * on the CPU at an anchor near the work (a tile's centre, the camera) and
 * handed to the GPU as f32 offsets from it (tiles-api.js's batch table).
 *
 * Math is the hardware's fast path: on the GPU the driver may fuse and
 * reorder float arithmetic and the f32 transcendentals (sin, exp, pow,
 * inverse square root, ...) are the device's own; on the CPU multiply-adds
 * fuse into FMAs. So a kernel's two builds agree to a few f32 ulps, not to
 * the bit (brokernel README.md).
 *
 * Mounted in every realm (the page's and a Worker's) when `BRO_WITH_KERNEL`
 * is enabled (every profile, minimal included); a build without it has
 * `bro.kernel.available === false`. The GPU half is the main thread's only:
 * a Worker has `bro.kernel.gpu === false`.
 */

console.log('brokernel', bro.kernel.version);

// ============================================================================
// 1. Compiling: bro.kernel.compile(source, { file, modules, knobs, enableProbes }) -> KernelModule
// ============================================================================
//
// Every kernel of the source is compiled, synchronously. An import resolves
// against the standard library (`std::math`, `std::noise`, `std::filter`,
// `std::sphere`, `std::color`), the `modules` map given (`{ name: source }`),
// any modules registered via `bro.kernel.registerModule(name, source)`, and
// the app's module resolver installed via `bro.kernel.resolve(fn)` (which is
// also shared with scene shader hooks in `createMesh` / `createVolume` /
// `setShaderModule`). `file` names the source in diagnostics.
//
// `knobs` (`{ NAME: number }`) overrides `@knob const NAME: T = default;`
// constants at compile time; `kernel.knobs(overrides)` recompiles a kernel
// with new knob values in place, `mod.withKnobs(overrides)` makes a new
// module with them.
//
// `pub const` values declared in the module are exposed on `mod.consts`
// (`mod.consts.SEA_LEVEL`), `mod.knobs` the `@knob` ones as compiled, and
// `mod.fn(name)` gives a `pub fn` as a KernelFn (`eval` / `run` / `dispatch`
// / `probe` / `knobs`, batching the arguments passed as stores).
//
// `mod.file` is the file it was compiled as, `mod.imports` the modules it
// imported, directly or not (sorted names).
//
// bro.kernel.reload({ name: source }) -> [{ file, kernels }]
//   New sources for modules already compiled: every live module whose file
//   is one of the names, or that imported a module of one, recompiles with
//   them (every other module keeps the source it was compiled with), all of
//   them or none (the first compile error throws and nothing changes). Its
//   Kernels and KernelFns follow in place at their next use, a kernel's own
//   `knobs` kept; a registered module of a name takes the new source too, so
//   later compiles and scene hooks see it. Returns each module recompiled,
//   `kernels` the names whose code changed (their SPIR-V, or for a @cpu
//   kernel its lowered code), so a caller redoes only what those produced:
//
//     const text = readApp('relief.bk');
//     for (const { file, kernels } of bro.kernel.reload({ 'planet::relief': text }))
//         if (file === 'planet.bk' && kernels.length) cache.refresh();
//     scene.reloadShaders({ 'planet::relief': text });   // the hooks importing it
//
// A compile error throws an Error whose message is the formatted diagnostics
// and whose `diagnostics` is [{ severity, file, line, column, message }]
// (f64 reached from a kernel that is not @cpu is one, at the f64).
//
// On the main thread a compile runs on a thread of its own while the window
// keeps answering. The script still waits for it. A module the compile needs
// from `bro.kernel.resolve` is asked of the resolver on the main thread. The
// same applies to `withKnobs`, `knobs`, `fn` and `reload`. A Worker compiles
// inline.
//
// bro.kernel.compileAsync(source, opts) -> Promise<KernelModule>
//   `compile` without the wait: it takes the same options and resolves to
//   the same module. It rejects with the same Error `compile` would throw,
//   bad arguments included. It starts at once, so any number compile in
//   parallel, and settles in a frame. Headless settles every pending one in
//   the frame after it was asked for. Start a world's kernels together and
//   await them:
//
//     const [relief, climate] = await Promise.all(
//         ['relief.bk', 'climate.bk'].map((f) => bro.kernel.compileAsync(read(f), { file: f, modules })));
//
// Compiles are cached on disk (docs/code-cache.md "Kernel cache"). A launch
// with unchanged sources takes each from the cache, with the same result.
//
// A @cpu kernel's CPU build (lowered and compiled to machine code, which the
// cache does not hold) starts on a thread of its own as its compile ends, on
// the page thread and with compileAsync alike: the module comes back without
// waiting for it. Its first eval or run (or cpuLanes read) waits for it as a
// compile waits, the window answering; a large kernel takes a second or more.
// That wait blocks the page's frame: a page that would go on drawing while
// it is made awaits the module's cpuReady() first.
//
// module.cpuReady() -> Promise<void>
//   Settles once every @cpu kernel of the module has its CPU build (at once
//   when it has none, or they are built); the frames go on meanwhile.
//   Headless settles it in the frame after it was asked for.
//
//     const m = await bro.kernel.compileAsync(source, { file, modules });
//     await m.cpuReady();          // m's evals and runs no longer wait

const mod = bro.kernel.compile(`
    import std::noise::*;
    from world::shape import ridge;

    @knob const AMP: f32 = 1.0;
    pub const SEA_LEVEL: f32 = 0.0;

    // A point kernel: no grid; called once per position. f64 positions far
    // from the origin: a @cpu kernel, with no GPU build.
    @kernel @cpu
    fn k_height(x: f64, y: f64, z: f64, seed: i32) -> f32 {
        let p = vec3<f64>(x, y, z) * 0.0005;
        return AMP * ridge(fbm_simplex<f64, 3>(p, 6, 2.0, 0.5, 0.0, seed) as f32);
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
    knobs: { AMP: 1.5 },
});

mod.kernels;                    // ['k_height', 'k_smooth', 'k_lookup']
mod.consts;                     // { SEA_LEVEL: 0 }
const height = mod.kernel('k_height');   // a Kernel; throws for an unknown name

// ============================================================================
// 2. A Kernel's description, knobs, probes, and stats
// ============================================================================

height.name;        // 'k_height'
height.gridDim;     // 0 for a point kernel, 1 to 3 for a grid kernel
height.returns;     // 'f32', or null for a kernel that returns nothing
height.cpuLanes;    // cells a grid kernel's CPU build computes at once (8 or 4 with AVX2, else 1);
                    // a kernel built for the GPU makes its CPU build when first run, eval'd or read here
height.params;      // what a call passes, in order (the grid parameter is not passed):
// [{ name: 'x', kind: 'scalar', type: 'f64' }, ..., { name: 'seed', kind: 'scalar', type: 'i32' }]
// A store's entry also has `access` ('read' | 'write' | 'read_write'), `dimension`,
// and `cells`: the grid axes the kernel accesses it at (below), 0 for none.

// kernel.stats() -> { spirvBytes, instructions, arithmetic, samples, loads,
//                     stores, branches, loops, functions: [{ name, self, total, share }],
//                     executables: [{ name, description, stats }] }
//   Static SPIR-V instruction breakdown, plus the stage by source function:
//   every call is inlined, so each function's `self` counts the instructions
//   its own body built, `total` adds its callees', `share` is total over the
//   whole stage; largest total first, the kernel itself (everything) first of
//   all. Plus hardware
//   pipeline executable statistics (registers, spills, etc.) when VK_KHR_pipeline_executable_properties
//   is supported by the GPU.
//
// kernel.knobs(overrides)
//   Recompiles the kernel in place with updated `@knob const` values (kept
//   across bro.kernel.reload).
//
// kernel.probe(coord, grid, ...args) -> { [probeName]: number | number[] }
//   Runs the kernel with `probe("name", expr)` instrumentation active and
//   returns the probed values at `coord` (x, [x, y], or [x, y, z]).

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
// reach buffers by address and compute in 64-bit integers). The work runs in
// the engine's graphics stream, in the order it is asked for, before any
// later frame's draws. A @cpu kernel has no GPU build: its dispatch throws.
//
// bro.kernel.createBuffer(byteLength | typedArray) -> KernelBuffer: GPU
// memory, reading zero, or holding the typed array's bytes. A KernelBuffer is
// untyped bytes; a store's elements are its byteLength / the element size.
//   buffer.byteLength
//   buffer.write(typedArray, byteOffset = 0)   // copied now, lands before later dispatches
//   buffer.read(typedArray, byteOffset = 0)    // fills it after every dispatch so far
//                                              // (blocks until they are done); returns it
//   buffer.readLater(byteOffset = 0, byteLength = rest) -> KernelReadback
//                                              // the same bytes, copied without waiting:
//     readback.ready                           // true once the GPU has made the copy
//     readback.byteLength
//     readback.read(typedArray)                // fills it from the copy's start; null until ready
//     readback.release()                       // drops its host memory now (else at collection)
//   buffer.destroy()                           // releases its GPU memory once in-flight work finishes;
//                                              // any later use of the buffer throws
//
// kernel.dispatch(grid, ...args) takes run()'s arguments with a KernelBuffer
// for each store. A kernel that returns a value returns a new KernelBuffer of
// one value per cell, laid out as run()'s typed array; one that returns
// nothing returns undefined. Stores are bounded as on the CPU (section 5).
// Each dispatch sees every earlier one's writes, and dispatches that touch
// no common bytes (neither writes what the other reads or writes) run side
// by side on the GPU: a stream waits only where a store's range, or a
// buffer's write or read, meets one written (or, for a write, read) since
// its last wait. Many small independent dispatches therefore cost about
// their longest, not their sum; a kernel with image, sampler, uniform or
// bound-buffer parameters waits for everything before it, and everything
// after waits for it.
//
// A scene draws from a KernelBuffer in place, with no read back: a node's
// custom shader declares `@binding(4, 14) var heights: read_buffer<f32>;`
// and the node binds the buffer with setShaderBuffer('heights', buffer)
// (scene-nodes-api.js).
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
// 7. bro.kernel.background(fn) -> BackgroundJob: work between the frames
// ============================================================================
//
// fn() is called at once, and every dispatch, write and createBuffer it makes
// goes to the background stream (render::VulkanCompute::beginBackground):
// held until the frame's GPU work is all submitted (its present, or at the
// latest the next frame's start), then run on the GPU's async compute queue
// (in order on the graphics queue where the device has none) after
// everything submitted before it, in order with earlier background jobs: in
// the time the frame leaves before the next one, not beside its draws. For
// work whose results are wanted frames later, not this frame: a world's
// field rebuilt into the half of a double buffer it does not draw from. A
// few small dependent dispatches are far from filling the GPU, so in the
// frame's own stream they lengthen the frame by all of their latency. (A
// tile cache's production runs here already, tiles-api.js.) A job may take
// many frames (a world's rivers built at start, thousands of dispatches):
// on an async queue the frames go on beside it, wait for it nowhere, and the
// page polls `done` each frame. Its work goes to the GPU 64 dispatches a
// submission, in order, as the frame's own stream does: one submission of
// thousands would run past the OS's GPU watchdog (about 2 s on Windows) and
// lose the device.
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

// ============================================================================
// 8. canvas.getContext('kernel'): a KernelBuffer shown in the page
// ============================================================================
//
// A <canvas> whose context is 'kernel' shows the pixels of a KernelBuffer: an
// app's own 2D GPU drawing (bro.image.gpu's colormap and fbm2D are made this
// way, image-gpu-api.js). Nothing is read back: present() records a store of
// the buffer's texels into the image the canvas's compositor layer samples
// (a compute pass, at the GPU's bandwidth), in the graphics stream after
// every dispatch so far, and the next frame shows it.
// The canvas keeps showing it until the next present.
//
//   ctx.canvas
//   ctx.present(buffer, { format = 'rgba8', byteOffset = 0 })
//
// The buffer holds canvas.width × canvas.height texels from byteOffset (a
// multiple of the texel's size), rows top to bottom, packed tight (a grid
// kernel's [x, y] store over the canvas's size): 'rgba8' is one u32 a pixel,
// `r | g << 8 | b << 16 | a << 24`; 'rgba32f' a vec4<f32>. Colour is
// premultiplied by alpha. The texels are taken at present(), so the buffer
// may be dispatched into again at once.
//
// Main thread only, like the GPU half; null in an iframe's document and on
// a canvas that has another context (and the other contexts are null on it).
// Errors: a non-KernelBuffer or an unknown format throws a TypeError; texels
// past the buffer's end or a bad byteOffset (negative, fractional, not a
// whole texel) a RangeError; present() inside
// bro.kernel.background a TypeError (the copy belongs to the frame).

if (bro.kernel.gpu) {
    const draw = bro.kernel.compile(`
        @kernel @grid(2)
        fn k_rings(g: grid<2>, out: write_store<u32, 2>, t: f32) {
            let d = length(vec2<f32>(g.x() as f32 - 160.0, g.y() as f32 - 120.0));
            let v = (sin(d * 0.1 - t) * 0.5 + 0.5) * 255.0;
            out[g.x(), g.y()] = (v as u32) | ((v as u32) << 8u) | (255u << 16u) | (255u << 24u);
        }
    `).kernel('k_rings');
    const canvas = document.querySelector('canvas');   // width 320, height 240
    const ctx = canvas.getContext('kernel');
    const pixels = bro.kernel.createBuffer(320 * 240 * 4);
    function tick(ms) {
        draw.dispatch([320, 240], pixels, ms * 0.004);
        ctx.present(pixels);
        requestAnimationFrame(tick);
    }
    requestAnimationFrame(tick);
}
