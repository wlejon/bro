// A rebuilt list gives its elements back.
//
// The two tests beside this one — test_create_remove_cycle and
// test_orphan_cleanup — check that removing elements does not *crash*. Neither
// checks that anything is reclaimed, and for a long time nothing was: the strong
// __bro_elem_map held every wrapper it ever cached, so the element finalizer
// that calls Document::freeNode could never run, and every element a list
// rebuild detached was retained for the life of the process. An application
// redrawing a panel off a progress number reached gigabytes and a window that
// spent most of each second in the sweep and the GC.
//
// So this asserts the reclamation, and it asserts the two things that must keep
// working around it: a detached element JS still holds stays usable and
// re-insertable, and an element back in the tree keeps the listeners registered
// on its wrapper.
//
// `__bro_elem_map` is the probe because an entry in it is exactly "this Element
// is still allocated and rooted". `advanceTime` rather than `flush` because the
// sweep and JS_RunGC are on the engine's once-a-second pass, and `flush` is
// layout and render only.

const root = document.getElementById('root');
const held = () => Object.keys(globalThis.__bro_elem_map || {}).length;

/// Two seconds: the sweep demotes on one pass and the GC that follows it
/// collects, so one pass is enough — two leaves no doubt.
const settle = () => advanceTime(2000);

function fill(n) {
    for (let i = 0; i < n; i++) {
        const row = document.createElement('div');
        row.className = 'row';
        row.appendChild(document.createElement('span'));
        root.appendChild(row);
    }
}

settle();
const base = held();

// ── 1. removeChild in a loop — what a DOM helper's put() does ──────────────

const CYCLES = 50;
const ROWS = 10;
for (let c = 0; c < CYCLES; c++) {
    while (root.firstChild) root.removeChild(root.firstChild);
    fill(ROWS);
    flush();
}
while (root.firstChild) root.removeChild(root.firstChild);
settle();

const built = CYCLES * ROWS * 2;
console.log(`  built ${built} elements per pass, held at rest ${base}`);
console.log(`  removeChild:      ${held() - base} still held`);
assert(held() - base < built / 10,
       `removeChild: ${held() - base} of ${built} rebuilt elements still held`);

// ── 2. innerHTML and replaceChildren detach the same way ───────────────────

for (let c = 0; c < CYCLES; c++) { root.innerHTML = ''; fill(ROWS); flush(); }
root.innerHTML = '';
settle();
console.log(`  innerHTML='':     ${held() - base} still held`);
assert(held() - base < built / 10,
       `innerHTML: ${held() - base} of ${built} rebuilt elements still held`);

for (let c = 0; c < CYCLES; c++) { root.replaceChildren(); fill(ROWS); flush(); }
root.replaceChildren();
settle();
console.log(`  replaceChildren:  ${held() - base} still held`);
assert(held() - base < built / 10,
       `replaceChildren: ${held() - base} of ${built} rebuilt elements still held`);

// ── 3. an element created and never appended is not kept either ────────────

for (let i = 0; i < 500; i++) {
    const d = document.createElement('div');
    d.className = 'never-used';
}
settle();
assert(held() - base < 50, `createElement: ${held() - base} unappended elements held`);

// ── 4. what must NOT be reclaimed: an element JS is still holding ──────────
//
// The reason this is a demotion and not a free. A live <video> or a canvas kept
// across a rebuild is the documented case, and it has to come back working.

const keep = document.createElement('div');
keep.id = 'kept';
keep.textContent = 'still here';
keep.dataset.mine = 'yes';
root.appendChild(keep);
flush();

let clicks = 0;
keep.addEventListener('click', () => { clicks++; });

root.removeChild(keep);
settle();
settle();

assert(keep.textContent === 'still here', 'a detached element JS holds still reads');
assert(keep.dataset.mine === 'yes', 'and keeps what was put on it');

root.appendChild(keep);
flush();
assert(root.children.length === 1, 'and goes back into the tree');
assert(document.getElementById('kept') === keep, 'and is findable again');

// ── 4b. a held element inside a container nobody holds ─────────────────────
//
// The case that a whole application suite found and this did not: a card is
// rebuilt, and something is holding the <video> *inside* the old card rather
// than the card. Freeing the card took the element with it, and the holder was
// left with a wrapper whose node was gone — `cannot read property of undefined`
// two frames later, on a redraw. So a doomed element gives up its children.

const box = document.createElement('div');
const inner = document.createElement('video');
inner.id = 'inner-kept';
box.appendChild(inner);
root.appendChild(box);
flush();

// Only the child is held from here on; the box is reachable from nothing.
root.removeChild(box);
settle();
settle();
settle();

assert(inner.id === 'inner-kept', 'a held child of a discarded parent survives');
assert(inner.parentNode === null || inner.parentNode !== undefined,
       'and can still be asked about its parent');
root.appendChild(inner);
flush();
assert(document.getElementById('inner-kept') === inner,
       'and goes back into the tree');
root.removeChild(inner);

// ── 5. and keeps its listeners, which live on the wrapper ──────────────────
//
// This is what the sweep's second pass is for: re-rooting an element that came
// back, before a collection can take __bro_listeners with it.

settle();
settle();
keep.dispatchEvent(new Event('click', { bubbles: true }));
assert(clicks === 1, `a re-attached element keeps its listeners (fired ${clicks})`);

// ── 6. and the whole thing still works afterwards ──────────────────────────

root.innerHTML = '';
fill(3);
flush();
assert(root.children.length === 3, 'the list still rebuilds at the end');
root.innerHTML = '';
