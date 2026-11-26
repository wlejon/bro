// bro.dynamics: bodies, joints, what goes through a joint, and a joint that breaks.
//
// - a box falls as far as gravity takes it, and comes to rest on a fixed one;
// - a welded body holds, and the weld carries its weight;
// - a weld made to take less than the weight breaks in the first step, is named by
//   broken() and is gone; one made to take more holds;
// - moved() names the bodies a step moved; removed bodies and joints are gone.
// Exercises src/dynamics/world.cpp + src/bronze_host/native_dynamics.cpp + js/dynamics.js.

assert(typeof bro.dynamics === 'object', 'bro.dynamics exists');
assert(typeof bro.dynamics.createWorld === 'function', 'createWorld exists');

function near(a, b, eps, msg) {
    assert(Math.abs(a - b) <= eps, msg + ' (got ' + a + ', want ' + b + ')');
}

// --- free fall, then rest ---------------------------------------------------
{
    const w = bro.dynamics.createWorld();
    const ground = w.addBox({ position: [0, -1, 0], half: [20, 1, 20], mass: 0 });
    const box = w.addBox({ position: [0, 10, 0], half: [0.5, 0.5, 0.5], mass: 100 });
    near(w.mass(box), 100, 1e-3, 'a box has the mass it was given');
    near(w.mass(ground), 0, 1e-6, 'a fixed body has none');
    for (let i = 0; i < 60; i++) w.step(1 / 60, 4);
    near(w.pose(box)[1], 10 - 0.5 * 9.81, 0.1, 'a second of falling');
    near(w.velocity(box)[1], -9.81, 0.05, 'and its speed');
    const m = w.moved();
    assert(m.length === 9 && m[0] === box, 'moved() names the one body that moved');
    for (let i = 0; i < 240; i++) w.step(1 / 60, 4);
    near(w.pose(box)[1], 0.5, 0.02, 'it rests on the ground');
    assert(Math.hypot(...w.velocity(box).slice(0, 3)) < 0.01, 'still');
    const c = w.counts;
    assert(c.bodies === 2 && c.joints === 0, 'counts');
    w.removeBody(box);
    assert(w.counts.bodies === 1, 'a removed body is gone');
    w.destroy();
    console.log('PASS fall and rest');
}

// --- a weld carries the weight ------------------------------------------------
{
    const w = bro.dynamics.createWorld();
    const beam = w.addBox({ position: [0, 5, 0], half: [1, 0.25, 0.25], mass: 1000 });
    const j = w.addJoint({ kind: 'weld', a: beam, anchor: [0, 5, 0] });
    for (let i = 0; i < 120; i++) w.step(1 / 60, 4);
    near(w.pose(beam)[1], 5, 0.01, 'a welded body stays');
    const load = w.jointLoad(j);
    near(Math.abs(load.force[1]), 9810, 100, 'the weld carries the weight');
    assert(w.hasJoint(j) && w.broken().length === 0, 'and holds');
    w.removeJoint(j);
    assert(!w.hasJoint(j) && w.counts.joints === 0, 'a removed joint is gone');
    for (let i = 0; i < 30; i++) w.step(1 / 60, 4);
    assert(w.pose(beam)[1] < 4.5, 'and the body falls');
    w.destroy();
    console.log('PASS weld load');
}

// --- a joint breaks where more goes through it than it takes ------------------------
{
    const w = bro.dynamics.createWorld();
    const weak = w.addBox({ position: [-3, 5, 0], half: [0.5, 0.5, 0.5], mass: 1000 });
    const strong = w.addBox({ position: [3, 5, 0], half: [0.5, 0.5, 0.5], mass: 1000 });
    const jw = w.addJoint({ kind: 'weld', a: weak, anchor: [-3, 5, 0], breakForce: 5000 });
    const js = w.addJoint({ kind: 'weld', a: strong, anchor: [3, 5, 0], breakForce: 20000 });
    let brokeAt = -1;
    for (let i = 0; i < 60; i++) {
        w.step(1 / 60, 4);
        const b = w.broken();
        if (b.length && brokeAt < 0) { brokeAt = i; assert(b.length === 1 && b[0] === jw, 'broken() names the weak joint'); }
    }
    assert(brokeAt >= 0 && brokeAt < 5, 'the weak joint broke at once (step ' + brokeAt + ')');
    assert(!w.hasJoint(jw) && w.hasJoint(js), 'the strong one holds');
    assert(w.pose(weak)[1] < 2 && Math.abs(w.pose(strong)[1] - 5) < 0.01, 'one fell, one stayed');
    w.destroy();
    console.log('PASS break by load');
}

// --- a hinge turns about its axis only -----------------------------------------
{
    const w = bro.dynamics.createWorld();
    const arm = w.addBox({ position: [1, 5, 0], half: [1, 0.1, 0.1], mass: 10 });
    w.addJoint({ kind: 'hinge', a: arm, anchor: [0, 5, 0], axis: [0, 0, 1] });
    let far = 0;
    for (let i = 0; i < 120; i++) {
        w.step(1 / 60, 4);
        const p = w.pose(arm);
        far = Math.max(far, Math.abs(Math.hypot(p[0], p[1] - 5) - 1), Math.abs(p[2]));
    }
    assert(far < 0.02, 'the arm swings a metre from the pin, in its plane (off by ' + far.toFixed(4) + ')');
    assert(w.pose(arm)[1] < 4.9, 'and it did swing');
    w.destroy();
    console.log('PASS hinge');
}
