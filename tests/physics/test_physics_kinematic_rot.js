// Physics.moveKinematic: the 9-argument form (tag, x, y, z, qx, qy, qz, qw, dt)
// moves AND rotates a kinematic body; the 5-argument form keeps the rotation.

if (!Physics || Physics.available === false) {
    console.log('physics unavailable; skipping');
} else {
    assert(Physics.available === true, 'Physics.available is true when compiled in');
    const id = Physics.createBody({
        shape: 'box', halfExtents: { x: 0.5, y: 0.5, z: 0.5 },
        position: { x: 0, y: 5, z: 0 },
    });
    Physics.setKinematic(id);

    // 90 degrees about Y: q = (0, sin45, 0, cos45).
    const s = Math.SQRT1_2;
    Physics.moveKinematic(id, 2, 5, 0, 0, s, 0, s, 1 / 60);
    Physics.step(1 / 60);
    let t = Physics.getTransform(id);
    let p = t.position, q = t.rotation;
    assert(Math.abs(p.x - 2) < 0.05, 'rotating move reaches x=2, got ' + p.x);
    assert(Math.abs(Math.abs(q.y) - s) < 0.05 && Math.abs(Math.abs(q.w) - s) < 0.05,
           '9-arg moveKinematic rotated 90deg about Y, got ' + JSON.stringify(q));

    // The 5-arg form leaves the rotation where it was.
    Physics.moveKinematic(id, 3, 5, 0, 1 / 60);
    Physics.step(1 / 60);
    t = Physics.getTransform(id);
    p = t.position; q = t.rotation;
    assert(Math.abs(p.x - 3) < 0.05, '5-arg moveKinematic reaches x=3, got ' + p.x);
    assert(Math.abs(Math.abs(q.y) - s) < 0.05, '5-arg moveKinematic keeps the rotation, got ' + JSON.stringify(q));

    // A zero quaternion is not a rotation: the body keeps its current one.
    Physics.moveKinematic(id, 4, 5, 0, 0, 0, 0, 0, 1 / 60);
    Physics.step(1 / 60);
    q = Physics.getTransform(id).rotation;
    assert(Math.abs(Math.abs(q.y) - s) < 0.05, 'zero quaternion ignored, rotation kept');

    let threw = false;
    try { Physics.moveKinematic(id, 1, 2, 3); } catch (e) { threw = true; }
    assert(threw, 'moveKinematic without dt throws');

    console.log('kinematic rotation OK');
}
