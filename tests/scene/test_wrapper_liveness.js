// Scene JS-wrapper liveness — stale GraphWrapper/NodeWrapper/TweenWrapper
// safety. Wrappers hold {weak liveness token, id} and re-resolve through the
// graph on every call (src/js/scene_bindings_internal.h), so a destroyed
// node/tween/graph must read as gone through EVERY wrapper of it, on every
// destruction path: direct destroy via another wrapper, ancestor subtree
// destroy, and whole-graph teardown when the canvas is detached and pruned.
// Every stale call below used to be a use-after-free.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '128');
canvas.setAttribute('height', '128');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
if (!scene) {
    console.log('scene context not available (no GPU)');
} else {
    // =====================================================================
    // Node destroyed through a DIFFERENT wrapper of the same node
    // =====================================================================
    const parent = scene.createNode('parent');
    const child = scene.createNode('child');
    parent.add(child);
    const childId = child.id;
    assert(childId > 0, 'live child has an id');

    const childAgain = scene.findById(childId);   // second wrapper, same node
    assert(childAgain !== null && childAgain.id === childId,
        'findById mints a second wrapper to the same node');

    childAgain.destroy();
    assert(scene.findById(childId) === null, 'node erased from the graph');
    assert(child.id === 0, 'original wrapper resolves dead after destroy via the other wrapper');
    child.x = 5;                                   // setter no-ops, must not crash
    assert(child.x === 0, 'stale wrapper x reads 0');
    assert(child.name === '', 'stale wrapper name reads empty');
    assert(child.position === undefined, 'stale wrapper position reads undefined');
    child.destroy();                               // double destroy is a no-op

    // =====================================================================
    // Ancestor subtree destroy invalidates descendant wrappers
    // =====================================================================
    const top = scene.createNode('top');
    const mid = scene.createNode('mid');
    const leaf = scene.createSprite({ name: 'leaf' });
    top.add(mid);
    mid.add(leaf);
    const midId = mid.id, leafId = leaf.id;

    top.destroy();
    assert(scene.findById(midId) === null, 'mid destroyed with subtree');
    assert(scene.findById(leafId) === null, 'leaf destroyed with subtree');
    assert(mid.id === 0, 'mid wrapper dead after ancestor destroy');
    assert(leaf.id === 0, 'leaf wrapper dead after ancestor destroy');
    leaf.play('walk');                             // sprite method on dead wrapper: no-op
    assert(leaf.children.length === 0, 'children of dead wrapper is empty array');
    assert(leaf.parent === null, 'parent of dead wrapper is null');
    mid.add(leaf);                                 // dead + dead: no-op, no crash
    mid.remove(leaf);

    // =====================================================================
    // Tween whose target node dies mid-flight keeps ticking safely,
    // and tween wrappers survive tween destruction
    // =====================================================================
    const n3 = scene.createNode('n3');
    const tw = scene.createTween();
    tw.to(n3, { position: [1, 2, 3] }, 0.2).start();
    n3.destroy();
    advanceTime(500);                              // tick past the tween's end
    assert(tw.isRunning === false, 'tween over a destroyed node finished');

    tw.destroy();
    assert(tw.isRunning === false, 'destroyed tween reads not running');
    let threw = false;
    try { tw.start(); } catch (e) { threw = true; }
    assert(threw, 'start() on a destroyed tween throws cleanly');

    // =====================================================================
    // Whole-graph teardown: detach the canvas, flush -> engine prunes the
    // graph. Retained graph/node/tween wrappers must all read as dead.
    // =====================================================================
    const keepRoot = scene.root;
    const keepNode = scene.createMesh({ mesh: 'box', color: 'red' });
    const keepNodeId = keepNode.id;
    const keepTween = scene.createTween();
    keepTween.to(keepNode, { scale: 2 }, 1).start();

    document.body.removeChild(canvas);
    flush();                                       // prune runs here

    // Graph wrapper: every entry point no-ops or returns null-ish.
    assert(scene.root === undefined, 'root of dead graph is undefined');
    assert(scene.createNode('x') === undefined, 'createNode on dead graph is undefined');
    assert(scene.createMesh({ mesh: 'box' }) === undefined, 'createMesh on dead graph is undefined');
    assert(scene.createTween() === undefined, 'createTween on dead graph is undefined');
    assert(scene.findById(keepNodeId) === null, 'findById on dead graph is null');
    assert(scene.findByName('anything') === null, 'findByName on dead graph is null');
    assert(scene.cullStats() === null, 'cullStats on dead graph is null');
    assert(scene.viewMatrix === null, 'viewMatrix of dead graph is null');
    assert(scene.cameraX === 0, 'cameraX of dead graph reads 0');
    scene.cameraX = 10;                            // setter no-ops
    scene.setCamera({ fov: 60, position: [1, 1, 1], target: [0, 0, 0] });
    scene.syncPhysics();
    scene.destroyNode(keepNode);

    // Node wrapper minted before teardown.
    assert(keepNode.id === 0, 'node wrapper dead after graph teardown');
    assert(keepNode.type === undefined, 'type of dead node is undefined');
    keepNode.position = [9, 9, 9];                 // no-op
    keepNode.destroy();                            // no-op
    assert(keepRoot.id === 0, 'root wrapper dead after graph teardown');

    // Tween wrapper minted before teardown.
    assert(keepTween.isRunning === false, 'tween of dead graph reads not running');
    threw = false;
    try { keepTween.to(keepNode, { scale: 1 }, 0.1); } catch (e) { threw = true; }
    assert(threw, 'to() on a tween of a dead graph throws cleanly');
    keepTween.stop();                              // silent no-op path

    // Time keeps advancing without the graph: nothing left to crash on.
    advanceTime(300);

    console.log('wrapper liveness: all stale-wrapper calls handled cleanly');
}
