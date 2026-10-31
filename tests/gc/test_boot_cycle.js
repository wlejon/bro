// The boot's collection is a marking cycle, not a stop-the-world full.
//
// hostCollectBetweenFrames (src/bronze_host/host_gc.cpp) collects what the
// page built before its first frame in the first between-frames slot. A page
// that builds a large heap in that turn made it one full collection of all of
// it in the first frame (94 ms at 1 GB). Incrementally (BRO_GC_INCREMENTAL,
// the default) the slot starts a marking cycle instead, once any sweep the
// boot left is done, and the cycle is paced over the frames like any other.
// This builds old data in the script's own turn, then checks that the first
// frames open a cycle, that it finishes across frames, and that no full
// collection ran but the cycle's.
//
// perf.stats().bronze: gcCycleOpen (a cycle the driver opened is marking),
// gcCycles (cycles finished), gcCycleFrames (the last one's frames),
// gcFullCollections (a cycle counts one).

const bz = () => perf.stats().bronze;

// Some 30 MB that stays live, built in this turn: the minors it runs promote
// it, so the old generation the boot's collection meets is large.
const keep = [];
for (let i = 0; i < 200000; i++) keep.push({ i, s: 'k' + (i & 1023), a: [i, i + 1, i + 2] });

// A page that animates: never quiescent, so no idle collection stops the world.
let frame = 0;
function tick() {
    frame++;
    requestAnimationFrame(tick);
}
requestAnimationFrame(tick);

const full0 = bz().gcFullCollections;
const cycles0 = bz().gcCycles;
let waited = 0;
while (waited < 30 && !bz().gcCycleOpen && bz().gcCycles === cycles0) {
    runFrames(1, { stepMs: 16 });
    waited++;
}
const opened = bz();
console.log(`  boot cycle open after ${waited} frames (open ${opened.gcCycleOpen}, finished ${opened.gcCycles - cycles0})`);
assert(opened.gcCycleOpen === 1 || opened.gcCycles > cycles0, `the boot's slot started a marking cycle (${waited} frames)`);
assert(opened.gcFullCollections - full0 === opened.gcCycles - cycles0,
       `no stop-the-world full collection at the boot (${opened.gcFullCollections - full0} full)`);

let ran = 0;
while (ran < 3000 && bz().gcCycles === cycles0) {
    runFrames(20, { stepMs: 16 });
    ran += 20;
}
const after = bz();
console.log(`  boot cycle finished after ${ran} more frames, over ${after.gcCycleFrames} frames`);
assert(after.gcCycles > cycles0, `the boot cycle finished between frames (${ran} frames run)`);
assert(after.gcCycleFrames > 1, `the cycle ran across frames (${after.gcCycleFrames})`);
assert(after.gcFullCollections - full0 === after.gcCycles - cycles0,
       `every full collection was a cycle (${after.gcFullCollections - full0} full, ${after.gcCycles - cycles0} cycles)`);
assert(keep.length === 200000 && keep[123456].a[2] === 123458, 'what the page kept is intact');
