// Body options the bronze binding once dropped: `dofs` ('2d' and the token
// list) and the chain shape's points / depth.

assert(typeof Physics === 'object', 'Physics namespace exists');

function world(g) {
    return Physics.createWorldHandle({ maxBodies: 256, gravity: g || { x: 0, y: 0, z: 0 } });
}
function stepN(w, n) { for (let i = 0; i < n; i++) w.step(1 / 60); }
function pos(w, tag) { return w.getTransform(tag).position; }

// -- dofs ------------------------------------------------------------------
{
    const w = world();
    const s = w.createBody({ shape: 'sphere', radius: 0.5, dofs: '2d' });
    assert(s > 0, "dofs '2d' body created");
    w.setLinearVelocity(s, 1, 0, 5);
    stepN(w, 60);
    const p = pos(w, s);
    assert(Math.abs(p.z) < 1e-3, "dofs '2d' holds z at 0 (z=" + p.z.toFixed(4) + ')');
    assert(Math.abs(p.x - 1) < 0.05, "dofs '2d' still moves in x (x=" + p.x.toFixed(3) + ')');

    const t = w.createBody({ shape: 'sphere', radius: 0.5, position: { x: 5, y: 0, z: 0 },
        dofs: 'tx, rz' });
    w.setLinearVelocity(t, 1, 2, 3);
    stepN(w, 60);
    const q = pos(w, t);
    assert(Math.abs(q.y) < 1e-3 && Math.abs(q.z) < 1e-3 && Math.abs(q.x - 6) < 0.05,
        "dofs 'tx, rz' frees only x translation (" + q.x.toFixed(3) + ',' + q.y.toFixed(3) + ',' + q.z.toFixed(3) + ')');

    assert(w.createBody({ shape: 'sphere', dofs: 'tq' }) === -1, 'an unknown dofs token is an error');
    w.destroy();
}

// -- chain -----------------------------------------------------------------
{
    const w = world({ x: 0, y: -9.81, z: 0 });
    const ground = w.createBody({ shape: 'chain', points: [-10, 0, 10, 0], depth: 4 });
    assert(ground > 0, 'chain body created');
    const ball = w.createBody({ shape: 'sphere', radius: 0.5, position: { x: 0, y: 3, z: 0 } });
    stepN(w, 120);
    const y = pos(w, ball).y;
    assert(y > 0.3 && y < 0.7, 'ball rests on the chain (y=' + y.toFixed(3) + ')');

    // depth: a ball outside the 4 m strip (|z| > 2) falls past it.
    const off = w.createBody({ shape: 'sphere', radius: 0.5, position: { x: 0, y: 3, z: 3 } });
    stepN(w, 120);
    assert(pos(w, off).y < -1, 'depth bounds the strip in z');

    assert(w.createBody({ shape: 'chain', points: [0, 0] }) === -1, 'a one-point chain is an error');
    w.destroy();
}

console.log('test_body_dofs_chain: OK');
