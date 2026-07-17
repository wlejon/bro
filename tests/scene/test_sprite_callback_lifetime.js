// Sprite animation-end callback lifetime. The JS callback is owned by the
// SpriteNode itself (a JSFnRef inside the std::function — see
// scene_bindings_fx.cpp), replacing a process-global registry keyed by node
// id that leaked its entries when a sprite died via ancestor subtree destroy
// or whole-graph prune. The real gate is the Debug build's QuickJS
// leaked-object assertion at engine teardown: every path below installs a
// callback and then kills the sprite through a path the old registry never
// swept.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '128');
canvas.setAttribute('height', '128');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
if (!scene) {
    console.log('scene context not available (no GPU)');
} else {
    const mkSprite = () => scene.createSprite({
        sheet: { frameWidth: 8, frameHeight: 8, columns: 2, rows: 1 },
        animations: { once: { frames: [0, 1], fps: 30, loop: false } },
    });

    // ---------------------------------------------------------------------
    // Callback fires with the clip name (baseline behavior unchanged).
    // ---------------------------------------------------------------------
    const sp1 = mkSprite();
    let ended = null;
    sp1.onAnimationEnd = (name) => { ended = name; };
    sp1.play('once');
    advanceTime(200);
    assert(ended === 'once', 'animation-end callback fired with clip name');

    // Reassigning clears the previous ref; null uninstalls.
    sp1.onAnimationEnd = (name) => { ended = 'second:' + name; };
    sp1.play('once');
    advanceTime(200);
    assert(ended === 'second:once', 'reassigned callback replaces the old one');
    sp1.onAnimationEnd = null;
    ended = null;
    sp1.play('once');
    advanceTime(200);
    assert(ended === null, 'null uninstalls the callback');

    // ---------------------------------------------------------------------
    // Ancestor subtree destroy: sprite dies with its parent while a callback
    // is installed. Old registry leaked this entry (only the directly-named
    // node was swept). Callback must never fire afterwards.
    // ---------------------------------------------------------------------
    const holder = scene.createNode('holder');
    const sp2 = mkSprite();
    holder.add(sp2);
    let fired2 = 0;
    sp2.onAnimationEnd = () => { fired2++; };
    sp2.play('once');
    holder.destroy();
    advanceTime(300);
    assert(fired2 === 0, 'no callback from a sprite destroyed via ancestor');
    assert(sp2.id === 0, 'sprite wrapper dead after ancestor destroy');

    // ---------------------------------------------------------------------
    // Callback destroys its own node mid-fire: the node copies the callable
    // before invoking, so self-destruction from inside is safe.
    // ---------------------------------------------------------------------
    const sp3 = mkSprite();
    let fired3 = 0;
    sp3.onAnimationEnd = () => { fired3++; sp3.destroy(); };
    sp3.play('once');
    advanceTime(300);
    assert(fired3 === 1, 'self-destroying callback fired exactly once');
    assert(sp3.id === 0, 'sprite destroyed from inside its own end callback');

    // ---------------------------------------------------------------------
    // Whole-graph prune with a live callback installed: detach the canvas
    // and flush. The old registry had NO sweep on this path at all — the
    // dup'd JSValue leaked and Debug teardown asserted.
    // ---------------------------------------------------------------------
    const sp4 = mkSprite();
    sp4.onAnimationEnd = () => {};
    sp4.play('once');
    document.body.removeChild(canvas);
    flush();                                  // graph pruned; sprite + JSFnRef freed
    advanceTime(200);
    assert(sp4.id === 0, 'sprite wrapper dead after graph prune');
    sp4.onAnimationEnd = () => {};            // install on dead wrapper: no-op

    console.log('sprite callback lifetime: all destruction paths clean');
}
