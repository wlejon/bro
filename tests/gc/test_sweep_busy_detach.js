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

// Warm-up, then a sample every ~0.25 s for ~5 s. The counts are a sawtooth
// (trees pile up, a collection takes them back every ~2 s), so a sample taken
// just before a collection is high whatever the trend: the checks read the
// whole run, not two points of it.
runFrames(60);
const samples = [];
for (let s = 0; s < 20; s++) {
    runFrames(15);
    const d = dom();
    samples.push({ nodes: d.nodes, entries: d.entries, freed: d.treesFreed, died: d.groupsDied,
                   sweepCollections: d.collections, mb: bytes() / 1048576 });
}
clearInterval(timer);
const show = (k) => samples.map((s) => (k === 'mb' ? s[k].toFixed(0) : s[k])).join(' ');
console.log(`  nodes ${show('nodes')}`);
console.log(`  entries ${show('entries')}`);
console.log(`  trees freed ${show('freed')}  sweep collections ${show('sweepCollections')}  private MB ${show('mb')}`);
console.log(`  ${built} panels built (${built * 61} nodes detached)`);

assert(built > 200, 'the timer rebuilt the panel every frame: ' + built);
const firstHalf = samples.slice(0, 10), secondHalf = samples.slice(10);
const min = (a, k) => Math.min(...a.map((s) => s[k]));
const max = (a, k) => Math.max(...a.map((s) => s[k]));
assert(samples[samples.length - 1].freed > samples[0].freed,
    `detached trees are freed while the page stays busy (${samples[0].freed} -> ${samples[samples.length - 1].freed})`);
// A plateau: what the documents own falls back after a collection (some sample
// at least halves the one before it; without collections the count only
// climbs) and the low point late in the run is no higher than the peak early on.
// The peak itself is a busy period's worth (~2 s of panels), whatever the run's
// length, so it is not bounded against the total.
const slack = 61 * 200;
const dropped = samples.some((s, i) => i > 0 && s.nodes < samples[i - 1].nodes / 2);
assert(dropped && min(secondHalf, 'nodes') <= max(firstHalf, 'nodes'),
    `the nodes the documents own plateau: ${show('nodes')} of ${built * 61} detached`);
assert(min(secondHalf, 'entries') <= max(firstHalf, 'entries') + slack,
    `registry entries plateau: ${show('entries')}`);
assert(min(secondHalf, 'mb') < min(firstHalf, 'mb') + 96, `private memory plateaus: ${show('mb')} MB`);
root.replaceChildren();
