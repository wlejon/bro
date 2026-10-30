// The old generation collected incrementally between frames.
//
// hostCollectBetweenFrames (src/bronze_host/host_gc.cpp) starts a marking
// cycle once the old generation has grown partway to its threshold, marks in
// a slice each frame, finishes the cycle in a frame of its own and sweeps in
// slices, rather than collecting the old generation in one pause. This runs
// a page that keeps promoting data it then drops, in the windowed frame loop
// (runFrames, where the between-frames collection runs), and checks that:
//
//   1. cycles run, each across several frames, and are the only full
//      collections the page gets (no stop-the-world one between frames);
//   2. what the page dropped is reclaimed: a WeakRef to an old object it let
//      go reads undefined after the cycles, and the heap stays bounded;
//   3. a detached DOM tree is still given back (the detached-tree sweep
//      learns what died from the cycles' finishes, not only from a
//      stop-the-world collection).
//
// perf.stats().bronze: gcCycles (cycles finished), gcCycleFrames (the last
// one's frames from start to finish), gcFullCollections (a cycle counts one).

const root = document.getElementById('root') || document.body;
const bz = () => perf.stats().bronze;

// Each frame keeps a batch of objects for RING frames, then drops it: they
// live long enough to be promoted, so the old generation fills with garbage
// at a steady rate and the cycles have work.
const RING = 120;
const ring = [];
let frame = 0;
let churn = true;
function tick() {
    if (churn) {
        const batch = new Array(1500);
        for (let i = 0; i < 1500; i++) batch[i] = { f: frame, i, s: 'b' + (i & 255), a: [i, frame] };
        ring.push(batch);
        if (ring.length > RING) ring.shift();
    }
    frame++;
    requestAnimationFrame(tick);
}
requestAnimationFrame(tick);

// Runs frames until `done()` or `max` frames; returns the frames run.
function framesUntil(done, max) {
    let n = 0;
    while (n < max && !done()) {
        runFrames(20, { stepMs: 16 });
        n += 20;
    }
    return n;
}

// ── 1. cycles run across frames, and are the page's full collections ───────

// Past the boot collection first.
runFrames(10, { stepMs: 16 });
const cycles0 = bz().gcCycles;
const full0 = bz().gcFullCollections;

// Old objects the page drops after they are promoted.
let dropped = [];
for (let i = 0; i < 2000; i++) dropped.push({ i, payload: [i, i + 1, i + 2] });
const wrDropped = new WeakRef(dropped[1234]);
runFrames(40, { stepMs: 16 });  // promoted by the minors these frames run

const ran = framesUntil(() => bz().gcCycles >= cycles0 + 1, 3000);
const after1 = bz();
console.log(`  first cycle after ${ran} frames: ${after1.gcCycles - cycles0} cycles, ` +
            `the last over ${after1.gcCycleFrames} frames`);
assert(after1.gcCycles >= cycles0 + 1, `a marking cycle finished between frames (${ran} frames run)`);
assert(after1.gcCycleFrames > 1, `the cycle ran across frames (${after1.gcCycleFrames})`);

// ── 2. what the page dropped is reclaimed ─────────────────────────────────

dropped = null;
const cycles1 = bz().gcCycles;
// The cycle open when it was dropped may keep it (it was live at its
// start); the one after cannot.
framesUntil(() => bz().gcCycles >= cycles1 + 2, 4000);
const after2 = bz();
assert(after2.gcCycles >= cycles1 + 2, `two more cycles finished (${after2.gcCycles - cycles1})`);
assert(wrDropped.deref() === undefined, 'an old object the page dropped is reclaimed by the cycles');
assert(after2.gcFullCollections - full0 === after2.gcCycles - cycles0,
       `every full collection was a cycle: ${after2.gcFullCollections - full0} full, ` +
       `${after2.gcCycles - cycles0} cycles`);
const live = ring.length * 1500;
console.log(`  heap ${(after2.heapUsedBytes / 1048576).toFixed(1)} MB used with ~${live} objects live`);
assert(after2.heapUsedBytes < 256 * 1048576, `the heap stays bounded (${after2.heapUsedBytes} bytes)`);

// ── 3. a detached tree is given back by the cycles ─────────────────────────

const nodes = () => perf.stats().dom.nodes;
runFrames(20, { stepMs: 16 });
const base = nodes();
const ROWS = 300;
for (let i = 0; i < ROWS; i++) {
    const row = document.createElement('div');
    row.className = 'row';
    row.appendChild(document.createElement('span'));
    root.appendChild(row);
}
runFrames(4, { stepMs: 16 });
assert(nodes() - base >= ROWS * 2, `the rows are in the document (${nodes() - base})`);
while (root.firstChild) root.removeChild(root.firstChild);

const cycles2 = bz().gcCycles;
const full2 = bz().gcFullCollections;
// Under the sweep's own deadlines for collecting by itself (a few hundred
// nodes, well inside ten seconds of frames): the cycles are what decide them.
const waited = framesUntil(() => nodes() - base < ROWS / 10, 560);
const after3 = bz();
console.log(`  detached rows: ${nodes() - base} nodes still held after ${waited} frames, ` +
            `${after3.gcCycles - cycles2} cycles`);
assert(nodes() - base < ROWS / 10, `a detached tree is reclaimed (${nodes() - base} of ${ROWS * 2} nodes held)`);
assert(after3.gcFullCollections - full2 === after3.gcCycles - cycles2,
       'and no stop-the-world collection was needed for it');

churn = false;
