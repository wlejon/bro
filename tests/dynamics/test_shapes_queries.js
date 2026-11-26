// bro.dynamics: the shapes beside the box and the sphere, bodies moved by hand, what a step
// reports of bodies meeting, rays, layers, and motion held to a plane.
//
// - a capsule, a cylinder and a hull come to rest on a mesh at the heights their shapes give;
// - a kinematic body goes where moveTo sends it and carries what rests on it;
// - touched() names a pair in the step they meet, hits() the place and the closing speed;
// - raycast() finds the nearest body on the layers asked for;
// - bodies on layers that do not touch pass through each other;
// - a planar body stays in its plane.
// Exercises src/dynamics/world.cpp + src/bronze_host/native_dynamics.cpp + js/dynamics.js.

function near(a, b, eps, msg) {
    assert(Math.abs(a - b) <= eps, msg + ' (got ' + a + ', want ' + b + ')');
}

function settle(w, steps) {
    for (let i = 0; i < steps; i++) w.step(1 / 60, 4);
}

// --- shapes at rest on a mesh ---------------------------------------------------
{
    const w = bro.dynamics.createWorld();
    const floor = w.addMesh({
        positions: [-20, 0, -20, 20, 0, -20, 20, 0, 20, -20, 0, 20],
        indices: [0, 2, 1, 0, 3, 2],
    });
    near(w.mass(floor), 0, 1e-6, 'a mesh is fixed');
    const capsule = w.addCapsule({ position: [-4, 3, 0], radius: 0.3, halfHeight: 0.5, mass: 10 });
    const cylinder = w.addCylinder({ position: [0, 3, 0], radius: 0.5, halfHeight: 0.4, mass: 10 });
    const wedge = w.addHull({
        position: [4, 3, 0], mass: 10,
        points: [-1, 0, -1, 1, 0, -1, 1, 0, 1, -1, 0, 1, -1, 1, -1, -1, 1, 1],
    });
    near(w.mass(capsule), 10, 1e-2, 'a capsule has the mass it was given');
    near(w.mass(cylinder), 10, 1e-2, 'a cylinder too');
    near(w.mass(wedge), 10, 1e-2, 'and a hull');
    settle(w, 300);
    near(w.pose(capsule)[1], 0.8, 0.03, 'the capsule stands on its end');
    near(w.pose(cylinder)[1], 0.4, 0.03, 'the cylinder rests on its face');
    near(w.pose(wedge)[1], 0, 0.03, 'the wedge rests on its base');
    assert(w.hasBody(wedge), 'hasBody');
    w.removeBody(floor);
    assert(!w.hasBody(floor), 'a removed mesh is gone');
    settle(w, 30);
    assert(w.pose(cylinder)[1] < -0.2, 'and what stood on it falls');
    let threw = false;
    try { w.addHull({ points: [0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0] }); } catch (e) { threw = true; }
    assert(threw, 'points in a plane make no hull');
    w.destroy();
    console.log('PASS shapes');
}

// --- a body moved by hand carries what rests on it -------------------------------------
{
    const w = bro.dynamics.createWorld();
    const lift = w.addBox({ position: [0, 0, 0], half: [2, 0.25, 2], kinematic: true });
    const crate = w.addBox({ position: [0, 0.75, 0], half: [0.5, 0.5, 0.5], mass: 20, friction: 0.9 });
    settle(w, 30);
    for (let i = 1; i <= 120; i++) {
        w.moveTo(lift, [0, i / 60, 0, 0, 0, 0, 1], 1 / 60);
        w.step(1 / 60, 4);
    }
    near(w.pose(lift)[1], 2, 0.01, 'the lift is where it was sent');
    near(w.pose(crate)[1], 2.75, 0.05, 'and the crate rode it');
    w.destroy();
    console.log('PASS moved by hand');
}

// --- meeting: touched and hits ----------------------------------------------------------
{
    const w = bro.dynamics.createWorld();
    const ground = w.addBox({ position: [0, -1, 0], half: [20, 1, 20], mass: 0 });
    const ball = w.addSphere({ position: [0, 5, 0], radius: 0.5, mass: 5 });
    let touchedAt = -1, hit = null;
    for (let i = 0; i < 120; i++) {
        w.step(1 / 60, 4);
        const t = w.touched();
        if (t.length && touchedAt < 0) {
            touchedAt = i;
            assert(t.length === 2, 'one pair');
            assert((t[0] === ground && t[1] === ball) || (t[0] === ball && t[1] === ground), 'the ball and the ground');
        }
        const h = w.hits();
        if (h.length && !hit) hit = Array.from(h);
    }
    // 4.5 m at 9.81: 0.958 s, step 57.
    assert(touchedAt >= 54 && touchedAt <= 60, 'they meet when the fall says (step ' + touchedAt + ')');
    assert(hit && hit.length === 9, 'a hit is reported');
    near(hit[3], 0, 0.1, 'at the ground');
    near(Math.abs(hit[6]), 1, 0.01, 'along the vertical');
    near(hit[8], 9.4, 0.5, 'at the speed of the fall');
    w.destroy();
    console.log('PASS touched and hits');
}

// --- rays and layers ---------------------------------------------------------------------
{
    const w = bro.dynamics.createWorld();
    const ground = w.addBox({ position: [0, -1, 0], half: [20, 1, 20], mass: 0, layers: 1 });
    const ghost = w.addBox({ position: [0, 3, 0], half: [1, 0.25, 1], mass: 0, layers: 2 });
    const ball = w.addSphere({ position: [0, 6, 0], radius: 0.25, mass: 1, layers: 4, mask: 1 });
    w.step(1 / 60, 4);
    let h = w.raycast([0.5, 10, 0.5], [0, -1, 0], 50);
    assert(h && h.body === ghost, 'the nearest body down the ray');
    near(h.point[1], 3.25, 0.01, 'on its top face');
    near(h.distance, 6.75, 0.01, 'at its distance');
    near(h.normal[1], 1, 0.01, 'facing up');
    h = w.raycast([0.5, 10, 0.5], [0, -2, 0], 50, 1);
    assert(h && h.body === ground, 'only the layers asked for');
    near(h.point[1], 0, 0.01, 'the ground under it');
    assert(w.raycast([0.5, 10, 0.5], [0, 1, 0], 50) === null, 'nothing above');
    assert(w.raycast([0.5, 10, 0.5], [0, -1, 0], 2) === null, 'nothing within reach');
    settle(w, 240);
    near(w.pose(ball)[1], 0.25, 0.02, 'a body falls through what its mask leaves out');
    w.destroy();
    console.log('PASS rays and layers');
}

// --- held to a plane ----------------------------------------------------------------------
{
    const w = bro.dynamics.createWorld();
    w.addBox({ position: [0, -1, 0], half: [20, 1, 20], mass: 0, rotation: [0.0871557, 0, 0, 0.9961947] });
    const free = w.addSphere({ position: [-2, 1, 0], radius: 0.5, mass: 1 });
    const flat = w.addSphere({ position: [2, 1, 0], radius: 0.5, mass: 1, planar: true });
    settle(w, 120);
    assert(Math.abs(w.pose(free)[2]) > 1, 'a free ball rolls down the slope');
    near(w.pose(flat)[2], 0, 0.02, 'a planar one stays in its plane');
    w.destroy();
    console.log('PASS planar');
}
