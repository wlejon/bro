// An animated element freed after the animation tick named it, read again
// before the next tick.
//
// The web animation tick names the elements it animated (activeThisTick)
// and those it finished (finishedThisTick); the style pass, the promotion
// routing and a sub-document read them after the tick. Script that runs in
// the frame after the tick (an animation event's listener, a message, a
// task) can free an animated element there and then (a shadow root's
// innerHTML frees its old children; the frame's drain gives their memory
// back), and the lists held raw Element*: the next layout read before a
// tick (Engine::flushLayoutForRead -> Document::resolveStyles ->
// Element::notePathDirty) walked freed memory. A window crash in Small
// Worlds, from a rAF's canvas read. The lists now carry the record's
// generation-checked identity and resolve it when read, as a record's own
// element is.
//
// Freed memory can read as what it held, so run this with MallocScribble=1
// (macOS) or under ASan to see the old crash; without them it passes either
// way, and still runs the sequence.

const host = document.createElement('div');
document.body.appendChild(host);
const shadow = host.attachShadow({ mode: 'open' });
const probe = document.createElement('div');
probe.style.width = '10px';
document.body.appendChild(probe);

const ROWS = 20;
const panel = () => '<style>@keyframes pulse { from { opacity: 0.2 } to { opacity: 1 } }'
    + ' .row { animation: pulse 40ms infinite; }</style>'
    + '<div class="list">' + '<div class="row">row</div>'.repeat(ROWS) + '</div>';

let freed = 0, reads = 0;
for (let c = 0; c < 20; c++) {
    shadow.innerHTML = panel();
    // In the frame, after the tick listed the rows: the panel goes, freed.
    shadow.querySelector('.row').addEventListener('animationiteration', () => {
        if (shadow.querySelector('.row')) { shadow.innerHTML = ''; freed++; }
    });
    // Each step then a layout read before the next tick: a change, and a
    // read that lays it out.
    const read = () => {
        probe.style.height = (reads % 2) + 1 + 'px';
        reads += probe.getBoundingClientRect().width === 10 ? 1 : 0;
    };
    for (let f = 0; f < 6; f++) { flush(); read(); advanceTime(16); read(); }
}

// 2. A detached element a script animation keeps listed (a running web
// animation names its element whether or not it is connected), freed by the
// sweep once its wrapper is collected: an idle second's collection, in the
// frame after the tick, then the drain.
const nodesBefore = perf.stats().dom.nodes;
const list = document.createElement('div');
document.body.appendChild(list);
for (let c = 0; c < 8; c++) {
    (() => {
        for (let i = 0; i < 40; i++) {
            const row = document.createElement('div');
            row.textContent = 'row';
            list.appendChild(row);
            row.animate([{ opacity: 0 }, { opacity: 1 }], { duration: 100000 });
        }
    })();
    flush();
    list.textContent = '';
    for (let k = 0; k < 3; k++) {
        advanceTime(1100);
        probe.style.height = (reads % 2) + 1 + 'px';
        reads += probe.getBoundingClientRect().width === 10 ? 1 : 0;
    }
}
const heldAfter = perf.stats().dom.nodes - nodesBefore;

console.log(`  ${freed} panels freed in a frame after its tick, ${reads} reads, ${heldAfter} nodes of 640 animated rows held`);
assert(freed >= 10, `panels freed by an animation event's listener (${freed})`);
assert(probe.getBoundingClientRect().width === 10, 'the page lays out after');
console.log('PASS test_web_animations_freed');
