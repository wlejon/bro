// Regression: a wrapper handed out for an ALREADY-DOOMED node must not outlive
// the node's storage with a live opaque.
//
// When a handler removes its own subtree, Document::freeNode queues the nodes in
// pendingFrees_ and fireNodeFreed nulls every wrapper that exists AT THAT MOMENT.
// But the dispatch keeps unwinding the propagation path afterwards, and a later
// listener reading e.target re-enters wrapElement() for the doomed node — which
// hands back a fresh "transient" wrapper. That wrapper is registered in neither
// __bro_elem_map nor the detached-document registry, so fireNodeFreed can never
// reach it. Once drainPendingFrees() releases the storage, the transient
// wrapper's opaque dangles, and both getElement() and the element finalizer
// dereference freed memory to probe isAlive().
//
// (That probe is not a safety net: magic_ is stamped 0xDEAD by ~Element, so
// reading it IS the use-after-free. It usually "works" only because the freed
// page still happens to hold 0xDEAD — which is why this went unnoticed.)
//
// Debug builds catch the dereference outright via the freed-Element tripwire in
// dom/element.h; on any build the assertions below pin the required behaviour:
// the retained wrapper must go inert, not dangle.

const root = document.getElementById('root');

let captured = null;
let capturedChild = null;

// Bubble-phase listener ABOVE the subtree that removes itself. It runs after the
// removal, so wrapping e.target here is the "re-wrap a doomed node" path.
root.addEventListener('dblclick', (e) => {
    captured = e.target;
    capturedChild = e.target.parentNode;
});

const overlay = document.createElement('div');
overlay.id = 'ov';
overlay.style.cssText = 'position:fixed;left:0;top:0;width:400px;';
const row = document.createElement('div');
row.className = 'row';
row.style.cssText = 'height:20px;';
row.innerHTML = '<span class="id">row-0</span>';
row.addEventListener('dblclick', () => { overlay.remove(); });
overlay.appendChild(row);
root.appendChild(overlay);

flush();

const r = row.getBoundingClientRect();
const cx = r.left + r.width / 2;
const cy = r.top + r.height / 2;
mouseMove(cx, cy);
click(cx, cy);
click(cx, cy);
flush();

assert(document.getElementById('ov') === null, 'overlay removed by dblclick');
assert(captured !== null, 'bubble listener captured the doomed target');

// Several frames so Document::drainPendingFrees() definitely releases the
// storage the captured wrapper still points at.
for (let i = 0; i < 4; i++) { advanceTime(16); flush(); }

// The captured wrappers now refer to storage that has been destroyed. Reading
// any property routes through getElement(), which must NOT dereference the
// freed Element. Post-fix the opaque was nulled while the memory was still
// valid, so these resolve to the inert-wrapper defaults instead.
const tag = captured.tagName;
const cls = captured.className;
const parent = captured.parentNode;
const kids = captured.childNodes.length;
captured.setAttribute('data-x', '1');
captured.textContent;
if (capturedChild) {
    capturedChild.tagName;
    capturedChild.childNodes.length;
}

console.log('inert reads: tag=' + tag + ' cls=' + cls +
            ' parent=' + parent + ' kids=' + kids);

// Force the transient wrappers to be collected while the runtime is still up,
// so js_element_finalizer runs on them for real (engine shutdown short-circuits
// the finalizer, which is what hid this path).
captured = null;
capturedChild = null;
if (typeof gc === 'function') gc();
advanceTime(16);
flush();

console.log('PASS: transient wrapper for a doomed node goes inert, never dangles');
