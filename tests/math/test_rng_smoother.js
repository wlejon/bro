// bro.math.Rng — deterministic SplitMix64 generator, and bro.math.Smoother —
// one-pole exponential ramp. Brief coverage alongside SpatialHash3D since all
// three share the math_bindings.cpp module.

assert(bro.math.Rng === Rng, 'Rng aliased onto bro.math');
assert(bro.math.Smoother === Smoother, 'Smoother aliased onto bro.math');

// ---- Rng: determinism -------------------------------------------------------

{
    const a = new Rng(1234);
    const b = new Rng(1234);
    const seqA = [];
    const seqB = [];
    for (let i = 0; i < 8; i++) seqA.push(a.float01());
    for (let i = 0; i < 8; i++) seqB.push(b.float01());
    for (let i = 0; i < 8; i++) {
        assert(seqA[i] === seqB[i], 'same seed produces identical sequence at index ' + i);
    }

    const c = new Rng(4321);
    let differs = false;
    for (let i = 0; i < 8; i++) {
        if (c.float01() !== seqA[i]) differs = true;
    }
    assert(differs, 'different seed produces a different sequence');
}

// ---- Rng: reseed resets the stream ------------------------------------------

{
    const r = new Rng(77);
    const first = [r.float01(), r.float01(), r.float01()];
    r.reseed(77);
    const second = [r.float01(), r.float01(), r.float01()];
    for (let i = 0; i < 3; i++) {
        assert(first[i] === second[i], 'reseed(same seed) reproduces the sequence at index ' + i);
    }
}

// ---- Rng: range/bounds -------------------------------------------------------

{
    const r = new Rng(9);
    for (let i = 0; i < 200; i++) {
        const v01 = r.float01();
        assert(v01 >= 0 && v01 < 1, 'float01 in [0,1); got ' + v01);

        const vs = r.signed();
        assert(vs >= -1 && vs < 1, 'signed in [-1,1); got ' + vs);

        const vr = r.range(10, 20);
        assert(vr >= 10 && vr < 20, 'range(10,20) in bounds; got ' + vr);

        const vi = r.int(5, 5);
        assert(vi === 5, 'int(5,5) degenerate range returns 5; got ' + vi);

        const vi2 = r.int(-3, 3);
        assert(vi2 >= -3 && vi2 <= 3 && Number.isInteger(vi2), 'int(-3,3) in bounds; got ' + vi2);

        const u = r.uint32();
        assert(u >= 0 && u <= 4294967295, 'uint32 in range; got ' + u);
    }
}

// ---- Rng: unit disc / sphere shapes -----------------------------------------

{
    const r = new Rng(555);
    for (let i = 0; i < 50; i++) {
        const d = r.inUnitDisc();
        const dl = d.x * d.x + d.y * d.y;
        assert(dl <= 1.0 + 1e-5, 'inUnitDisc within unit disc; got r^2=' + dl);

        const s = r.inUnitSphere();
        const sl = s.x * s.x + s.y * s.y + s.z * s.z;
        assert(sl <= 1.0 + 1e-5, 'inUnitSphere within unit sphere; got r^2=' + sl);

        const os = r.onUnitSphere();
        const osl = os.x * os.x + os.y * os.y + os.z * os.z;
        assert(Math.abs(osl - 1.0) < 1e-3, 'onUnitSphere on the unit sphere surface; got r^2=' + osl);
    }
}

// ---- Smoother: converges toward target, snaps on reset ----------------------

{
    const s = new Smoother(200, 60); // 200ms time-constant at 60Hz
    assert(s.current === 0, 'Smoother starts at 0 current');
    assert(s.target === 0, 'Smoother starts at 0 target');

    s.reset(0);
    s.setTarget(1);
    assert(s.target === 1, 'setTarget updates target');

    let prev = s.current;
    for (let i = 0; i < 10; i++) {
        const v = s.tick();
        assert(v > prev, 'tick monotonically approaches target from below; step ' + i);
        assert(v <= 1, 'tick never overshoots the target; got ' + v);
        prev = v;
    }

    // After many ticks it should have closed most of the gap.
    const closed = s.tickN(500);
    assert(closed > 0.99, 'after many ticks, value has nearly reached target; got ' + closed);

    // reset() snaps current AND target with no ramp.
    s.reset(-5);
    assert(s.current === -5, 'reset snaps current; got ' + s.current);
    assert(s.target === -5, 'reset snaps target; got ' + s.target);
}
