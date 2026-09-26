// A UI that rebuilds from templates keeps flat memory.
//
// The shape of every view switch in an app that re-renders whole: clone a
// template, fill it, wire an onclick and a listener that close over the clone,
// append it, then throw the whole panel away and build the next. Before the
// detached-tree sweep (src/bronze_host/host_node_sweep.h) nothing a rebuild
// detached was ever given back — the node, its registry entry, its wrapper and
// the closures on it — about 50 KB a five-element card.
//
// Asserted on the engine's own counts (perf.stats().dom: nodes the documents
// own, registry entries, wrapped entries) over many rounds after a warm-up, so
// the numbers compare a steady state with itself.
//
// And the half that must NOT be reclaimed: a detached card the program keeps,
// and a node inside a discarded card the program keeps, come back whole —
// expandos, dataset, inline handler, listeners — after several sweeps.

const root = document.getElementById('root');

const tpl = document.createElement('template');
tpl.innerHTML =
    '<div class="card"><h3 class="t">T</h3><p class="b">B</p>' +
    '<button class="go">Go</button><span class="n"></span></div>';
document.body.appendChild(tpl);

const dom = () => perf.stats().dom;
const bytes = () => perf.stats().processBytes;
const settle = () => { advanceTime(1100); advanceTime(1100); advanceTime(1100); };

let clicks = 0;
let hovers = 0;

function card(i) {
    const c = tpl.content.cloneNode(true).firstElementChild;
    c.querySelector('.t').textContent = 'Card ' + i;
    c.querySelector('.b').textContent = 'Body of card ' + i;
    c.querySelector('.go').onclick = () => { clicks++; c.dataset.hit = String(i); };
    c.addEventListener('mouseenter', () => { hovers++; c.classList.add('hover'); });
    c.style.width = (100 + (i % 50)) + 'px';
    c.payload = { index: i, label: 'card-' + i };
    return c;
}

function round(n) {
    for (let i = 0; i < n; i++) root.appendChild(card(i));
    flush();
    root.replaceChildren();
}

const CARDS = 100;
const ROUNDS = 40;

// ── warm-up: every cache, pool and code path has run once ─────────────────
for (let r = 0; r < 5; r++) { round(CARDS); advanceTime(300); }
settle();
const d0 = dom();
const b0 = bytes();
console.log(`  at rest:  nodes ${d0.nodes}  entries ${d0.entries}  wrapped ${d0.wrapped}` +
            `  private ${(b0 / 1048576).toFixed(1)} MB`);

// ── the loop ───────────────────────────────────────────────────────────────
for (let r = 0; r < ROUNDS; r++) {
    round(CARDS);
    advanceTime(300);
}
settle();
const d1 = dom();
const b1 = bytes();
const built = ROUNDS * CARDS;
const grew = (b1 - b0) / 1048576;
console.log(`  after ${built} cards (${built * 5} elements):  nodes ${d1.nodes}` +
            `  entries ${d1.entries}  wrapped ${d1.wrapped}  private ${(b1 / 1048576).toFixed(1)} MB` +
            `  (+${grew.toFixed(1)} MB)`);
console.log(`  sweep: ${d1.passes} passes, ${d1.collections} collections,` +
            ` ${d1.groupsDied} groups freed, ${d1.treesFreed} trees freed,` +
            ` last pass ${d1.lastPassMs.toFixed(2)} ms, last collection ${d1.lastCollectMs.toFixed(1)} ms`);

// Unreclaimed, these rounds cost 36000 nodes, 20000 entries and about 250 MB.
// The private bytes are asserted too, with room for the allocator's own
// high-water: memory that grows with the rounds is a leak the counts above
// cannot see, such as every property name a host object is built with copied
// into the immortal string arena again. (A run from the repo root loads the
// system panels; a hidden one no longer animates, so it no longer commits
// memory of its own while this measures.)
assert(d1.nodes - d0.nodes < CARDS, `nodes grew by ${d1.nodes - d0.nodes} over ${built} cards`);
assert(b0 === 0 || grew < 32, `private memory grew by ${grew.toFixed(1)} MB over ${built} cards`);
// An entry whose last reference is a closure behind a style object goes when
// the collector finalizes that closure, which it may do a collection or two
// late: a round or two of cards, never the 20000 of a leak.
assert(d1.entries - d0.entries < 3 * CARDS, `entries grew by ${d1.entries - d0.entries} over ${built} cards`);
assert(d1.wrapped - d0.wrapped < CARDS, `wrapped grew by ${d1.wrapped - d0.wrapped} over ${built} cards`);

// ── a detached card the program keeps ──────────────────────────────────────
const kept = card(7);
kept.id = 'kept-card';
kept.dataset.mine = 'yes';
root.appendChild(kept);
flush();
root.removeChild(kept);
round(CARDS);
settle();
settle();

assert(kept.payload && kept.payload.label === 'card-7', 'a kept card keeps its expandos');
assert(kept.dataset.mine === 'yes', 'and its dataset');
assert(kept.querySelector('.t').textContent === 'Card 7', 'and its subtree');
root.appendChild(kept);
flush();
assert(document.getElementById('kept-card') === kept, 'and goes back into the tree');
kept.querySelector('.go').click();
assert(clicks === 1, `and its button's onclick fires (${clicks})`);
assert(kept.dataset.hit === '7', 'with the closure it was given');
kept.dispatchEvent(new Event('mouseenter'));
assert(hovers === 1 && kept.classList.contains('hover'), `and its listener fires (${hovers})`);
root.removeChild(kept);

// ── a node inside a card nobody keeps ──────────────────────────────────────
let button = null;
{
    const c = card(9);
    c.id = 'lost-card';
    root.appendChild(c);
    flush();
    button = c.querySelector('.go');
    button.tag = 'inner';
    root.replaceChildren();
}
round(CARDS);
settle();
settle();
assert(button.tag === 'inner', 'a kept inner node keeps its expandos');
assert(button.parentNode && button.parentNode.id === 'lost-card',
       'and the card around it, which it reaches');
root.appendChild(button.parentNode);
flush();
button.click();
assert(clicks === 2, `and its onclick still fires (${clicks})`);
assert(document.getElementById('lost-card').dataset.hit === '9', 'on the card it closed over');
root.replaceChildren();

// ── a canvas with a scene, thrown away ─────────────────────────────────────
const scenes0 = __host.sceneContextCount();
for (let i = 0; i < 8; i++) {
    const cv = document.createElement('canvas');
    cv.width = 64;
    cv.height = 64;
    root.appendChild(cv);
    const sc = cv.getContext('scene');
    if (sc && sc.createBox) sc.createBox({ size: 1 });
    flush();
    root.removeChild(cv);
}
settle();
const scenes1 = __host.sceneContextCount();
console.log(`  scenes: ${scenes0} before, ${scenes1} after 8 thrown away`);
assert(scenes1 <= scenes0, `thrown-away canvases keep ${scenes1 - scenes0} scenes`);

// ── and the tree still works ───────────────────────────────────────────────
round(3);
for (let i = 0; i < 3; i++) root.appendChild(card(i));
flush();
assert(root.children.length === 3, 'the list still rebuilds at the end');
root.replaceChildren();
