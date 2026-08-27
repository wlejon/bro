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
//
// It also pins two bugs the shared pick path fixed on the way. The mesh
// path built its local ray from the node LOCAL transform, so a mesh under a
// moved parent was tested at its local offset; and it converted distances by
// scale.x alone and dropped scale from the normal entirely, so a node scaled
// differently per axis reported the wrong distance, honoured the wrong
// maxDistance, and returned a normal that was not normal to its surface.

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


    // --- Parent transforms ---
    //
    // The mesh path used to build its local ray from node.position/rotation/
    // scale — the node LOCAL transform — so a mesh under a moved group was
    // tested at its local offset and picked where it does not draw. Both paths
    // now go through worldMatrix(), so a parent transform counts.
    const group = scene.createNode();
    group.position = [30, 0, 0];
    const child = scene.createMesh({ mesh: Mesh.box(1, 1, 1) });
    child.name = 'parented-child';
    child.position = [0, 3, 0];
    group.add(child);
    flush();

    // Draws at x=30 (group) + y=3 (child), half-extent 1, so its top is y=4.
    const atWorld = scene.raycast([30, 100, 0], [0, -1, 0]);
    assert(atWorld && atWorld.hit, 'a mesh under a moved parent is picked in world space');
    assert(Math.abs(atWorld.point[1] - 4) < 0.05,
        `parented mesh hit lands on its world top face y=4 (got ${atWorld.point[1]})`);
    assert(atWorld.node && atWorld.node.name === 'parented-child',
        `the hit names the child, not the group (got ${atWorld.node && atWorld.node.name})`);

    // Same for an instanced node under a parent.
    const igroup = scene.createNode();
    igroup.position = [0, 0, 40];
    const inst = scene.createInstancedMesh({ mesh: Mesh.box(0.5, 0.5, 0.5) });
    inst.setInstances(new Float32Array([
        1, 0, 0, 0,
        0, 1, 0, 2,
        0, 0, 1, 0,
        1, 1, 1, 1,
    ]));
    igroup.add(inst);
    flush();
    const iw = scene.raycast([0, 100, 40], [0, -1, 0]);
    assert(iw && iw.hit, 'an instance under a moved parent is picked in world space');
    assert(Math.abs(iw.point[1] - 2.5) < 0.05,
        `parented instance hit lands at y=2.5 (got ${iw.point[1]})`);
    assert(iw.instance === 0, 'a parented instanced hit still names its copy');

    // --- Non-uniform scale: distances ---
    //
    // The old local-max-distance conversion divided by scale.x alone, so a
    // node scaled differently per axis converted its BVH cutoff and its
    // reported distance with the wrong factor.
    const squash = scene.createMesh({ mesh: Mesh.box(1, 1, 1) });
    squash.position = [-30, 0, 0];
    squash.scale = [4, 0.25, 1];
    flush();

    const top = scene.raycast([-30, 100, 0], [0, -1, 0]);
    assert(top && top.hit, 'a non-uniformly scaled mesh is picked');
    assert(Math.abs(top.point[1] - 0.25) < 0.02,
        `the squashed top face sits at y=0.25 (got ${top.point[1]})`);
    assert(Math.abs(top.distance - 99.75) < 0.02,
        `distance is world units down the squashed axis (want 99.75, got ${top.distance})`);

    // The +X face is at x = -30 + 4 = -26; along the STRETCHED axis the old
    // scale.x divide happened to be right, so check the squashed axis above
    // and this one together — one factor has to serve both.
    const side = scene.raycast([100, 0, 0], [-1, 0, 0]);
    assert(side && side.hit, 'the side face of a stretched box is picked');
    assert(Math.abs(side.point[0] - (-26)) < 0.05,
        `the stretched +X face sits at x=-26 (got ${side.point[0]})`);
    assert(Math.abs(side.distance - 126) < 0.05,
        `distance is reported in world units (want 126, got ${side.distance})`);

    // A maxDistance cutoff has to be in world units too: the squashed box top
    // is 99.75 away, so 90 must miss and 105 must hit. Under the old scale.x
    // divide the cutoff handed to the BVH was off by 16x on this node.
    const tooShort = scene.raycast([-30, 100, 0], [0, -1, 0], 90);
    assert(!tooShort || !tooShort.hit, 'a maxDistance shorter than the hit misses');
    const longEnough = scene.raycast([-30, 100, 0], [0, -1, 0], 105);
    assert(longEnough && longEnough.hit, 'a maxDistance past the hit still hits');

    // --- Non-uniform scale: normals ---
    //
    // A normal transforms by the inverse-transpose, not the basis. The old
    // code rotated the local normal and dropped scale entirely. A BOX cannot
    // show the difference — its faces are axis-aligned in local space, so
    // every candidate transform agrees. An ellipsoid can: flatten a sphere and
    // an off-axis point tilts far closer to vertical than the sphere normal.
    const ell = scene.createMesh({ mesh: Mesh.sphere(1, 64, 32) });
    ell.position = [0, 0, -40];
    ell.scale = [1, 0.25, 1];
    flush();

    // Down the x=0.6 column: the unit sphere is struck at local (0.6, 0.8, 0),
    // whose local normal is that same direction. Scale y by 0.25 and the true
    // surface normal becomes S^-1 (0.6, 0.8, 0) normalized = (0.184, 0.983, 0).
    // The old scale-less transform would have reported (0.6, 0.8, 0) — a 37
    // degree error, and the discriminator here.
    const eh = scene.raycast([0.6, 100, -40], [0, -1, 0]);
    assert(eh && eh.hit, 'the flattened sphere is picked');
    assert(Math.abs(eh.point[1] - 0.2) < 0.02,
        `the flattened sphere surface sits at y=0.2 (got ${eh.point[1]})`);
    const en = eh.normal;
    assert(Math.abs(Math.hypot(en[0], en[1], en[2]) - 1) < 0.01,
        'the reported normal is unit length');
    assert(en[1] > 0.95,
        `flattening tilts the normal toward vertical (want >0.95, got ${en[1]})`);
    assert(Math.abs(en[0] - 0.184) < 0.05,
        `the normal is the inverse-transpose one (want ~0.184, got ${en[0]})`);

    console.log('instanced raycast OK');
}
