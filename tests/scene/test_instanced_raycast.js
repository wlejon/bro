// scene.raycast against INSTANCED geometry.
//
// An InstancedMeshNode is its own SceneNode::Type, so the raycast traversal —
// which tested Type::Mesh only — skipped every one of them. Nothing instanced
// could be picked: neither scene.createInstancedMesh nodes nor a TileWorld's
// object kinds (props, buildings), which are one InstancedMeshNode per kind.
// Callers were pushed onto TileWorld.raycastCell, which sees the tile height
// field alone and so looks straight through anything standing on it and
// answers with the ground behind — an error of √2 m of ground per metre of
// height at an isometric pitch.
//
// This pins the fix: a ray down the Y axis at an instance must return that
// instance, at its own height, with its index.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '256');
canvas.setAttribute('height', '256');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
if (!scene) {
    console.log('scene context not available (no GPU)');
} else {
    scene.setCamera({
        fov: 60, near: 0.1, far: 200,
        position: [0, 20, 20], target: [0, 0, 0], up: [0, 1, 0],
    });

    // A 1x1x1 box (half-extents 0.5), instanced at three separate columns.
    const box = Mesh.box(0.5, 0.5, 0.5);
    const node = scene.createInstancedMesh({ mesh: box });
    assert(node, 'createInstancedMesh returns a node');

    // Canonical 16-float record: 4x3 affine rows, then RGBA.
    const XS = [-6, 0, 6];
    const YS = [1, 5, 9];          // distinct heights, so a hit names its instance
    const data = [];
    for (let i = 0; i < XS.length; i++) {
        data.push(1, 0, 0, XS[i],
                  0, 1, 0, YS[i],
                  0, 0, 1, 0,
                  1, 1, 1, 1);
    }
    node.setInstances(new Float32Array(data));
    assert(node.instanceCount === 3, `instanceCount is 3 (got ${node.instanceCount})`);
    flush();

    // Straight down onto each instance in turn.
    for (let i = 0; i < XS.length; i++) {
        const hit = scene.raycast([XS[i], 100, 0], [0, -1, 0]);
        assert(hit && hit.hit, `instance ${i} is pickable at x=${XS[i]}`);
        const topY = YS[i] + 0.5;
        assert(Math.abs(hit.point[1] - topY) < 0.05,
            `instance ${i} hit lands on its top face y=${topY} (got ${hit.point[1]})`);
        assert(hit.instance === i,
            `the result names which copy was struck (want ${i}, got ${hit.instance})`);
        assert(hit.normal[1] > 0.9, `instance ${i} top-face normal points up`);
    }

    // A ray between the columns must miss rather than snap to the nearest.
    const miss = scene.raycast([3, 100, 0], [0, -1, 0]);
    assert(!miss || !miss.hit, 'a ray through the gap misses');

    // The nearest instance wins when several are in line: shoot along +X
    // through all three at the height of the middle one and expect the first
    // one the ray reaches, not the closest to the origin in some other sense.
    const inline = scene.raycast([-100, YS[1], 0], [1, 0, 0]);
    assert(inline && inline.hit, 'an axial ray hits an instance');
    assert(inline.instance === 1,
        `the ray stops at the first instance it reaches (got ${inline.instance})`);

    // Hiding the node hides it from picking, like any other geometry.
    node.visible = false;
    flush();
    const hidden = scene.raycast([XS[0], 100, 0], [0, -1, 0]);
    assert(!hidden || !hidden.hit, 'an invisible instanced node is not pickable');
    node.visible = true;
    flush();

    // A plain MeshNode nearer the camera still wins over an instance — the two
    // paths share one closest-hit comparison rather than one shadowing the other.
    const slab = scene.createMesh({ mesh: Mesh.box(1, 0.5, 1) });
    slab.position = [XS[0], YS[0] + 6, 0];
    flush();
    const over = scene.raycast([XS[0], 100, 0], [0, -1, 0]);
    assert(over && over.hit, 'the nearer plain mesh is hit');
    assert(Math.abs(over.point[1] - (YS[0] + 6.5)) < 0.05,
        `the nearer mesh wins over the instance (got y=${over.point[1]})`);
    assert(over.instance === undefined,
        'a non-instanced hit carries no instance index');

    console.log('instanced raycast OK');
}
