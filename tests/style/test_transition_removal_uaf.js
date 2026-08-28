// Removing an element that is running (or has run) a CSS transition or a CSS
// animation must not crash the engine.
//
// The transition and animation managers (src/engine/css_transitions.cpp) index
// their per-element records by raw dom::Element*, and tick() dereferences that
// key — markDirty() the moment a transition/animation completes — while the
// transitionend/animationend it queues carries the same pointer to
// dispatchEvent a few frames later. Nothing used to drop those records when an
// element went away, so an element removed mid-transition left a dangling key
// that the next tick walked into: a hard segfault, not a caught error. That is
// what a toast which fades itself out and then removes itself does on every
// single toast.
//
// Document::freeNode() now forgets the doomed node in both managers (and the
// wholesale paths — Document::parse(), ~Document — forget the whole document).
// This test is a crash test: every assert below is incidental. If the engine is
// broken the process dies before reaching them, and the runner reports the
// missing PASS line.

const root = document.getElementById('root');
root.innerHTML = '';

function box(css) {
    const b = document.createElement('div');
    b.style.cssText = 'width:60px;height:60px;background-color:rgb(255,0,0);opacity:1;' + css;
    root.appendChild(b);
    flush();
    return b;
}

// Beat the engine on past where the removed element's transition would have
// ended, in several ticks, and drain the deferred frees in between.
function settle(ms) {
    const step = 25;
    for (let t = 0; t < ms; t += step) { advanceTime(step); flush(); }
}

// --- 1. Removed MID-transition (the crashing case) -------------------------
{
    const b = box('transition:opacity 200ms linear;');
    b.style.opacity = '0';
    flush();
    advanceTime(100);        // half way
    flush();
    b.remove();              // remove() frees the Element (deferred)
    flush();
    settle(400);             // tick past the end it will never reach
    assert(root.childNodes.length === 0, 'mid-transition removal left no children');
}

// --- 2. Removed AFTER the transition completed -----------------------------
{
    const b = box('transition:opacity 150ms linear;');
    b.style.opacity = '0';
    flush();
    settle(300);             // transition finishes while attached
    b.remove();
    flush();
    settle(300);
    assert(root.childNodes.length === 0, 'post-transition removal left no children');
}

// --- 3. Transition on a CHILD of the removed subtree ------------------------
// freeNode() recurses depth-first, so the child's record must go too.
{
    const outer = document.createElement('div');
    const inner = document.createElement('div');
    inner.style.cssText = 'width:40px;height:40px;background-color:rgb(0,0,255);' +
                          'transition:opacity 200ms linear;opacity:1;';
    outer.appendChild(inner);
    root.appendChild(outer);
    flush();
    inner.style.opacity = '0';
    flush();
    advanceTime(80);
    flush();
    outer.remove();          // the transition is on the child, not on outer
    flush();
    settle(400);
    assert(root.childNodes.length === 0, 'subtree removal left no children');
}

// --- 4. innerHTML wipe of a subtree mid-transition --------------------------
// A different free path (js_element_set_innerHTML → freeNode per child), and
// the one a full-re-render UI actually takes.
{
    const holder = document.createElement('div');
    root.appendChild(holder);
    const kids = [];
    for (let i = 0; i < 4; ++i) {
        const k = document.createElement('div');
        k.style.cssText = 'width:20px;height:20px;background-color:rgb(255,0,0);' +
                          'transition:opacity 250ms linear,width 250ms linear;opacity:1;';
        holder.appendChild(k);
        kids.push(k);
    }
    flush();
    for (const k of kids) { k.style.opacity = '0'; k.style.width = '80px'; }
    flush();
    advanceTime(120);
    flush();
    holder.innerHTML = '';
    flush();
    settle(500);
    assert(holder.childNodes.length === 0, 'innerHTML wipe left no children');
    holder.remove();
    flush();
    settle(200);
}

// --- 5. A CSS ANIMATION interrupted by removal ------------------------------
// The animation manager keeps an element's record even after the animation
// completes (the previousName memo), so its keys outlive the animation itself
// and dangle just as readily.
{
    const sheet = document.createElement('style');
    sheet.textContent =
        '@keyframes uaf-fade { from { opacity: 1; } to { opacity: 0; } }';
    document.head.appendChild(sheet);
    flush();

    // removed mid-animation
    const a = box('animation:uaf-fade 200ms linear forwards;');
    advanceTime(90);
    flush();
    a.remove();
    flush();
    settle(400);

    // removed after the animation finished
    const c = box('animation:uaf-fade 120ms linear forwards;');
    settle(260);
    c.remove();
    flush();
    settle(260);

    assert(root.childNodes.length === 0, 'animation removals left no children');
    sheet.remove();
    flush();
}

// --- 6. Re-adding a removed element ----------------------------------------
// removeChild (unlike remove()) keeps the Element alive and re-insertable. Its
// transition record must survive the detach — or at minimum the element must
// still transition correctly once it is back in the tree.
{
    const b = box('transition:opacity 200ms linear;');
    b.style.opacity = '0';
    flush();
    advanceTime(100);
    flush();
    root.removeChild(b);
    flush();
    settle(300);
    root.appendChild(b);     // back in
    flush();
    b.style.transition = 'opacity 200ms linear';
    b.style.opacity = '1';
    flush();
    settle(400);
    const op = parseFloat(getComputedStyle(b).opacity);
    assert(op > 0.9, 're-added element finished its new transition, got ' + op);
    b.remove();
    flush();
    settle(300);
}

// --- 7. Many at once, staggered -- what a toast queue looks like ------------
{
    for (let i = 0; i < 12; ++i) {
        const t = box('transition:opacity 150ms ease-out,transform 150ms ease-out;');
        t.style.opacity = '0';
        t.style.transform = 'translateY(-10px)';
        flush();
        advanceTime(40 + (i % 5) * 30);   // remove at every phase of the fade
        flush();
        t.remove();
        flush();
    }
    settle(600);
    assert(root.childNodes.length === 0, 'staggered removals left no children');
}

root.innerHTML = '';
flush();
settle(200);

console.log('transition/animation removal: no use-after-free');
