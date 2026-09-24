// Scene node wrappers are identity-stable: every path that hands out a node
// answers the object the app already holds for it, so === works and expandos
// stick. Each native used to mint a fresh wrapper per read.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '128');
canvas.setAttribute('height', '128');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
if (!scene) {
    console.log('no scene; skipping node identity test');
} else {
    const box = scene.createMesh({ mesh: 'box', name: 'box' });
    box.tag = 'mine';
    assert(scene.findByName('box') === box, 'findByName answers the created node');
    assert(scene.findByName('box') === scene.findByName('box'), 'findByName is stable');
    assert(scene.findById(box.id) === box, 'findById answers the created node');
    assert(scene.findByName('box').tag === 'mine', 'expandos stick');

    const root = scene.root;
    assert(root === scene.root, 'root is stable');
    assert(box.parent === root, 'parent is the root wrapper');
    assert(root.children.includes(box), 'children holds the created node');

    const group = scene.createNode({ name: 'group' });
    group.add(box);
    assert(box.parent === group && group.children[0] === box, 'reparented: parent / children agree');

    const cam = scene.createCamera({ fov: 50, near: 0.1, far: 100, position: [0, 2, 6], target: [0, 0, 0], active: true });
    assert(scene.activeCamera === cam, 'activeCamera is the camera it was made');
    assert(scene.activeCamera === scene.activeCamera, 'activeCamera is stable');

    group.x = 0; box.x = 0;
    advanceTime(16);
    flush();
    const hit = scene.raycast([0, 2, 6], [0, -2 / Math.hypot(2, 6), -6 / Math.hypot(2, 6)], 100);
    assert(hit && hit.node === box, 'raycast hit.node is the created node');

    const sprite = scene.createSprite({ name: 'sprite', width: 8, height: 8 });
    assert(scene.findByName('sprite') === sprite, 'sprites too');
    assert(sprite.setPosition(1, 2, 0) === sprite, 'chainable setters return the same wrapper');

    // A node the app never held a wrapper for is stable from its first read.
    const html = scene.createHtmlNode({ html: '<p>x</p>', width: 32, height: 16, name: 'label' });
    assert(scene.findByName('label') === html, 'html node');

    // A destroyed node is gone, and a new node never aliases it.
    const id = box.id;
    box.destroy();
    assert(scene.findById(id) === null, 'destroyed node is not found');
    const again = scene.createMesh({ mesh: 'box', name: 'box' });
    assert(again !== box && scene.findByName('box') === again, 'a new node is a new wrapper');

    console.log('node identity: ok');
}
