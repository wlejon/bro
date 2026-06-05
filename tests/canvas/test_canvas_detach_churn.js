// Regression: rapidly creating and removing canvas-backed elements must not
// use-after-free the backing CanvasScene.
//
// When a <canvas> is removed from the DOM (e.g. via container.textContent = ''),
// its dom::Element is destroyed through the deferred-free path
// (Document::freeNode -> drainPendingFrees), NOT necessarily the JS GC
// finalizer. The CanvasScene holds layout/detached callbacks whose userdata is
// that Element. Element::~Element must fire CanvasScene::onElementFinalized so
// those callbacks are cleared before the memory is reclaimed — otherwise the
// next frame's rasterize()/prepareAndSignal() dereferences a freed Element and
// crashes. This mirrors the kokoro-lab "click random rapidly" crash.

var container = document.createElement('div');
document.body.appendChild(container);

function buildBatch(n) {
    for (var i = 0; i < n; i++) {
        var c = document.createElement('canvas');
        c.setAttribute('width', '64');
        c.setAttribute('height', '64');
        container.appendChild(c);
        var ctx = c.getContext('2d');
        assert(ctx !== null && ctx !== undefined, 'getContext 2d in batch');
        ctx.fillStyle = '#3366ff';
        ctx.fillRect(0, 0, 64, 64);
        ctx.fillStyle = '#ffffff';
        ctx.fillText('x', 10, 20);
    }
}

// Churn: build a batch of canvases, render, then blow them all away and rebuild
// — repeatedly. flush() runs the frame loop (record + rasterize + the
// deferred-free drain), which is where the dangling pointer used to be hit.
for (var round = 0; round < 12; round++) {
    buildBatch(9);
    flush();
    advanceTime(16);
    flush();

    // Drop every canvas at once — the textContent='' path that frees the
    // backing Elements out from under their still-live CanvasScenes.
    container.textContent = '';
    flush();
    advanceTime(16);
    flush();
}

// If we got here without crashing, the detach wiring held up.
assert(container.children.length === 0, 'container emptied after final churn');

// A freshly built canvas after all that churn must still work end to end.
buildBatch(1);
flush();
var fresh = container.querySelector('canvas');
assert(fresh !== null, 'fresh canvas present after churn');
var fctx = fresh.getContext('2d');
assert(fctx !== null && fctx !== undefined, 'fresh canvas getContext 2d works');
fctx.fillStyle = 'red';
fctx.fillRect(0, 0, 64, 64);
flush();

assert(true, 'canvas detach churn survived without use-after-free');
