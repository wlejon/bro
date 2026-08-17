// The resize probe: ResizeObserver over the bronze frame seam.
//
// Unlike MutationObserver there is nothing for the DOM to report here — a box
// changes size because a window resized, a font arrived, or a sibling grew, and
// none of those is a mutation. So this is a poll, and what has to be pinned is
// the poll's contract rather than any notification's:
//
//   1. The FIRST delivery reports the current size, unprompted. That initial
//      callback is why code reaches for a ResizeObserver instead of a resize
//      listener, and an implementation that only fired on change would look
//      correct until the very first layout.
//
//   2. A size that did not change is not reported. A poll that delivered every
//      frame would be indistinguishable from one that worked, and would run an
//      app's relayout callback sixty times a second forever.
//
//   3. unobserve and disconnect take effect immediately — before the next
//      poll, not after one more delivery.
//
// The frame at which each thing happens is load-bearing and is spelled out at
// the tick that does it: rAF runs at step 5 of the seam and the resize poll at
// step 5b, so a size changed from inside a tick is measured by the poll of the
// SAME frame.
//
// EVERY LINE IS `APP <name>=<value>`, every value an integer, a boolean or a
// string this file chose — the expectation beside it is written from what must
// be true, not recorded from a run (tests/bronze_host/README.md).

function say(label, value) { console.log('APP ' + label + '=' + value); }

function label(el) { return el.id ? '#' + el.id : 'el'; }

function entryText(e) {
    return label(e.target) +
           ' content=' + e.contentRect.width + 'x' + e.contentRect.height +
           ' border=' + e.borderBoxSize[0].inlineSize + 'x' + e.borderBoxSize[0].blockSize +
           ' box=' + e.contentBoxSize[0].inlineSize + 'x' + e.contentBoxSize[0].blockSize;
}

const a = document.getElementById('a');
const b = document.getElementById('b');
const c = document.getElementById('c');
say('fixture.found', (a !== null) && (b !== null) && (c !== null));

// ---------------------------------------------------------------------------
// The main observer: an initial report, then one more when the box changes
// ---------------------------------------------------------------------------

const lines = [];
let calls = 0;
let observerArgIsSelf = false;
let deliveredInTurn = false;
let turnOver = false;

const watcher = new ResizeObserver(function (entries, observer) {
    if (!turnOver) deliveredInTurn = true;
    calls++;
    observerArgIsSelf = observer === watcher;
    for (const e of entries) lines.push(entryText(e));
});
watcher.observe(a);

// ---------------------------------------------------------------------------
// unobserve: a target dropped before the first poll is never reported at all
// ---------------------------------------------------------------------------

let droppedCalls = 0;
const dropped = new ResizeObserver(function (entries) { droppedCalls += entries.length; });
dropped.observe(b);
dropped.unobserve(b);

// ---------------------------------------------------------------------------
// disconnect: the initial report arrives, and nothing after it
// ---------------------------------------------------------------------------

let cutCalls = 0;
const cut = new ResizeObserver(function (entries) { cutCalls += entries.length; });
cut.observe(c);

turnOver = true;

// ---------------------------------------------------------------------------
// The frames
// ---------------------------------------------------------------------------

let frames = 0;
function tick() {
    frames++;
    if (frames === 2) {
        // The poll runs later in THIS frame (seam step 5b, after rAF at 5), so
        // this is measured now rather than next frame.
        a.style.width = '200px';
        // And the third observer stops watching in the same breath, after its
        // initial report last frame: the resize below must not reach it.
        cut.disconnect();
        c.style.width = '90px';
    }
    if (frames === 3) {
        // Nothing changes this frame, or any frame after it. Claim 2.
        a.style.width = '200px';   // the same value: not a change
    }
    if (frames < 6) { requestAnimationFrame(tick); return; }

    say('sync.deliveredInTurn', deliveredInTurn);
    say('watch.calls', calls);
    say('watch.observerArg', observerArgIsSelf);
    say('watch.entries', lines.length);
    for (let i = 0; i < lines.length; i++) say('e' + i, lines[i]);
    say('dropped.calls', droppedCalls);
    say('cut.calls', cutCalls);

    // Printed last, so a probe that died halfway is a missing line rather than
    // a silently short but otherwise matching output.
    say('done', 1);
}
requestAnimationFrame(tick);
