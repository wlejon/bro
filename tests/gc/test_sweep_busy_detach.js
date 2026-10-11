// A page that detaches a tree every frame, and never goes quiet, is still
// given its memory back.
//
// The DOM sweep (src/bronze_host/host_node_sweep.h) asks for a collection of
// its own only after a second with no trees detached, so a view switch does
// not pay for one in its next frame. A page that never pauses (a timer
// rebuilding a panel every frame, as a virtualized list scrolling does) never
// gets that second; the heap's own collections, the memory-pressure GC and the
// sweep's long-wait rules are what reclaim it then. Over ~5 s of the real
// frame loop (runFrames) such a page must have its trees freed and its node,
// entry and private-memory counts plateau.

const root = document.getElementById('root');
const dom = () => perf.stats().dom;
const bytes = () => perf.stats().processBytes;

// A panel of 60 cards with listeners and expandos (wrapped, so each detached
// tree is demoted and has to be proven dead by a collection), rebuilt every
// frame from a timer: no rAF, so only the detaching keeps the page busy.
let built = 0;
function panel() {
    const p = document.createElement('div');
    for (let i = 0; i < 60; i++) {
        const c = document.createElement('div');
        c.className = 'card';
        c.textContent = 'card ' + built + '.' + i;
        c.addEventListener('click', () => { c.dataset.hit = '1'; });
        c.payload = { i, label: 'x'.repeat(64) };
        p.appendChild(c);
    }
    built++;
    return p;
}
const timer = setInterval(() => root.replaceChildren(panel()), 16);

// Warm-up, then four samples ~1.2 s apart.
runFrames(60);
const samples = [];
for (let s = 0; s < 4; s++) {
    runFrames(75);
    const d = dom();
    samples.push({ nodes: d.nodes, entries: d.entries, freed: d.treesFreed, died: d.groupsDied,
                   sweepCollections: d.collections, mb: bytes() / 1048576 });
}
clearInterval(timer);
for (const s of samples)
    console.log(`  nodes ${s.nodes}  entries ${s.entries}  trees freed ${s.freed}  groups died ${s.died}` +
                `  sweep collections ${s.sweepCollections}  private ${s.mb.toFixed(1)} MB`);
console.log(`  ${built} panels built (${built * 61} nodes detached)`);

assert(built > 200, 'the timer rebuilt the panel every frame: ' + built);
const first = samples[0], last = samples[samples.length - 1];
assert(last.freed > first.freed, `detached trees are freed while the page stays busy (${first.freed} -> ${last.freed})`);
// What one panel holds, and room for the trees still waiting on a collection,
// but not the ~${built * 61} nodes detached in all.
assert(last.nodes < first.nodes + 20000 && last.nodes < built * 61 / 2,
    `the nodes the documents own plateau: ${samples.map((s) => s.nodes).join(' -> ')} of ${built * 61} detached`);
assert(last.entries < first.entries + 20000,
    `registry entries plateau: ${samples.map((s) => s.entries).join(' -> ')}`);
assert(last.mb < first.mb + 96, `private memory plateaus: ${samples.map((s) => s.mb.toFixed(0)).join(' -> ')} MB`);
root.replaceChildren();
