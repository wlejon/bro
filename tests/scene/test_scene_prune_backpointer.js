// Reclaiming a scene graph must leave nothing behind on its canvas Element.
//
// The engine keeps two back-pointers on the canvas: sceneGraph() ("this element
// is a composited 3D layer") and sceneGraphFBOTexture() (the texture the
// compositor binds for it). draw_traversal.cpp reads the first to take a
// dedicated return path and emit a layer break, then hands the second to the
// compositor. Pruning the graph when the canvas leaves the DOM used to clear
// neither: the element went on claiming a scene that had been destroyed, which
// is a compositor layer break over a freed graph's texture and a
// use-after-free for the first reader that dereferences rather than tests.
//
// __host.sceneLink() is the only way to see those fields — they are opaque
// void*/GLuint on the C++ side with no DOM surface of their own.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '128');
canvas.setAttribute('height', '128');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
if (!scene) {
    console.log('scene context not available (no GPU)');
} else {
    scene.createMesh({ mesh: 'box', color: 'red' });
    flush();

    const registered = __host.sceneContextCount();
    assert(registered >= 1, 'engine registered the scene context');

    let link = __host.sceneLink(canvas);
    assert(link.graph === true, 'live graph: canvas carries the scene back-pointer');
    assert(link.fboTexture > 0, 'live graph: canvas carries an FBO texture id');

    // removeChild leaves the Element alive and re-insertable (unlike remove(),
    // which queues it for free), so the back-pointers can still be read after
    // the graph behind them is gone. That is exactly the window the bug lived in.
    document.body.removeChild(canvas);
    flush();                                   // prune runs here

    assert(__host.sceneContextCount() === registered - 1,
        'graph reclaimed when the canvas left the document');

    link = __host.sceneLink(canvas);
    assert(link.graph === false,
        'scene back-pointer cleared when the graph was reclaimed');
    assert(link.fboTexture === 0,
        'FBO texture id cleared when the graph was reclaimed');

    // Re-attaching must not resurrect a layer break for the destroyed graph:
    // the element is an ordinary canvas again until something asks for a new
    // context. A frame here is what would have composited the stale texture.
    document.body.appendChild(canvas);
    flush();
    link = __host.sceneLink(canvas);
    assert(link.graph === false, 'a re-attached canvas has no scene until asked again');
    assert(link.fboTexture === 0, 'and no FBO texture');

    // And asking again builds a genuinely new context rather than handing back
    // the reclaimed one — the flag being trustworthy is what lets getContext
    // tell "has a live graph" from "had one once".
    const scene2 = canvas.getContext('scene');
    assert(scene2 !== null && scene2 !== undefined, 'a fresh scene context is built');
    assert(__host.sceneContextCount() === registered,
        'exactly one context registered again, not two');
    assert(__host.sceneLink(canvas).graph === true, 'and the back-pointer is set again');

    console.log('scene prune back-pointer: cleared on reclaim, rebuildable after');
}
