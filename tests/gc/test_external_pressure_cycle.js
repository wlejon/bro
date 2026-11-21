// External memory collected by a cycle between frames, not a stop-the-world
// full in the middle of one.
//
// ArrayBuffers a Worker transfers in (a mesh, a decoded image) are external
// memory: the heap does not see their bytes. Past max(64 MB, live) of it made
// since the last ask, bronze asks for a full collection of the page's heap,
// which would stop the world at the page's next allocation, mid-frame. bro
// defers that ask (bronze embed::deferExternalPressure) and takes it between
// frames (hostCollectBetweenFrames, src/bronze_host/host_gc.cpp), where it
// starts an incremental marking cycle as memory pressure on a busy page does.
//
// This transfers an 8 MB buffer into the page every frame (through the clone
// a worker's postMessage crosses, __host.cloneBench) in the windowed frame
// loop (runFrames, where the between-frames collection runs), drops it, and
// checks that:
//   1. cycles run, and every full collection the page gets is a cycle's;
//   2. the buffers are given back: the process does not grow by what was made.

const bz = () => perf.stats().bronze;
const MB = 1 << 20;

let made = 0;
let on = true;
function tick() {
    if (on) {
        const buf = new ArrayBuffer(8 * MB);
        new Uint8Array(buf)[0] = 1;
        const r = __host.cloneBench(buf, [buf]);
        if (r && r.value) made += r.value.byteLength;
    }
    requestAnimationFrame(tick);
}
requestAnimationFrame(tick);

// Past the boot collection first.
runFrames(10, { stepMs: 16 });
const cycles0 = bz().gcCycles;
const full0 = bz().gcFullCollections;
const bytes0 = perf.stats().processBytes;
made = 0;

runFrames(300, { stepMs: 16 });
on = false;
runFrames(30, { stepMs: 16 });

const s = bz();
const cycles = s.gcCycles - cycles0;
const fulls = s.gcFullCollections - full0;
const grew = (perf.stats().processBytes - bytes0) / MB;
console.log(`  ${(made / MB).toFixed(0)} MB transferred in: ${cycles} cycles, ${fulls} full collections, process grew ${grew.toFixed(0)} MB`);

assert(made >= 2000 * MB, `the buffers were transferred in (${(made / MB).toFixed(0)} MB)`);
assert(cycles >= 2, `cycles collected the external memory (${cycles})`);
assert(fulls === cycles, `every full collection was a cycle's (${fulls} fulls, ${cycles} cycles)`);
assert(grew < 600, `the transferred buffers were given back (process grew ${grew.toFixed(0)} MB of ${(made / MB).toFixed(0)} MB made)`);
