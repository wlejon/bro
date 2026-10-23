// Symbolic Runtime & Prover Reflection APIs (#128, #129, #133, #135, #137):
//   - bro.solve / bro.solveAll / bro.solveWithCert (#128)
//   - bro.optimize (#129)
//   - bro.fuzz (#133)
//   - bro.prove (#135)
//   - bro.layout.solve (#137)

assert(typeof bro.solve === 'function', 'bro.solve is a function');
assert(typeof bro.solveAll === 'function', 'bro.solveAll is a function');
assert(typeof bro.solveWithCert === 'function', 'bro.solveWithCert is a function');
assert(typeof bro.optimize === 'function', 'bro.optimize is a function');
assert(typeof bro.fuzz === 'function', 'bro.fuzz is a function');
assert(typeof bro.prove === 'function', 'bro.prove is a function');
assert(typeof bro.layout === 'object' && typeof bro.layout.solve === 'function',
       'bro.layout.solve is a function');

// ---- #128: bro.solve / bro.solveAll / bro.solveWithCert ---------------------

{
    const sol = bro.solve((x, y) => (x * x + y * y === 25) && (x > y) && (y > 0), {
        bounds: { x: [0, 10], y: [0, 10] }
    });
    assert(sol !== null, 'bro.solve finds Pythagorean triple');
    assert(sol.x === 4 && sol.y === 3, 'expected x=4, y=3; got x=' + sol.x + ', y=' + sol.y);

    const unsat = bro.solve((x) => (x > 10) && (x < 5), {
        bounds: { x: [-50, 50] }
    });
    assert(unsat === null, 'bro.solve returns null on UNSAT');

    const cert = bro.solveWithCert((a) => ((a ^ 170) === 255) && (a >= 0) && (a <= 255));
    assert(cert.sat === true && cert.status === 'sat', 'solveWithCert status is sat');
    assert(cert.model.a === (255 ^ 170), 'solveWithCert model.a === 85');

    const all = bro.solveAll((x) => (x >= 1) && (x <= 4) && ((x & 1) === 1), 10, {
        bounds: { x: [0, 10] }
    });
    assert(Array.isArray(all) && all.length === 2, 'bro.solveAll enumerates 2 odd values in [1..4]');
    const vals = [all[0].x, all[1].x].sort((a, b) => a - b);
    assert(vals[0] === 1 && vals[1] === 3, 'bro.solveAll found 1 and 3');
}

// ---- #129: bro.optimize (CEGIS + SMT equivalence proof) ---------------------

{
    const fastFn = bro.optimize(function slowDist(x, y) {
        return (x + 0) * 1 + (y - 0);
    }, {
        bounds: { x: [-50, 50], y: [-50, 50] }
    });
    assert(typeof fastFn === 'function', 'bro.optimize returns callable function');
    assert(fastFn.verified === true, 'bro.optimize verified equivalence');
    assert(fastFn.optimized === true, 'bro.optimize synthesized simplified form');
    assert(fastFn(12, 30) === 42, 'optimized function executes accurately: got ' + fastFn(12, 30));
    assert(typeof fastFn.certificate === 'object' && fastFn.certificate.verified === true,
           'fastFn.certificate attached');
}

// ---- #133: bro.fuzz (White-box concolic bug hunter & path generator) --------

{
    const report = bro.fuzz(function riskyDivide(x, y) {
        if (x === 42) {
            return 100 / (y - 7);
        }
        if (x < 0) {
            return -x;
        }
        return x + y;
    }, {
        bounds: { x: [-50, 100], y: [-50, 100] }
    });

    assert(typeof report === 'object', 'bro.fuzz returns report object');
    assert(report.feasiblePaths >= 2, 'bro.fuzz explores feasible execution paths');
    assert(Array.isArray(report.tests) && report.tests.length >= 2, 'bro.fuzz synthesizes path-covering tests');
    assert(Array.isArray(report.bugs) && report.bugs.length >= 1, 'bro.fuzz discovers division-by-zero hazard');
}

// ---- #135: bro.prove (Runtime proof certificate & reflection API) -----------

{
    const cert = bro.prove(function safeClamp(x) {
        if (x < 0) return 0;
        if (x > 100) return 100;
        return x;
    });
    assert(typeof cert === 'object', 'bro.prove returns certificate object');
    assert(cert.status === 'VERIFIED', 'safeClamp is VERIFIED; got ' + cert.status);
    assert(cert.safe === true, 'safeClamp cert.safe === true');
    assert(cert.deterministic === true, 'safeClamp cert.deterministic === true');
    assert(cert.proofHash !== '0x0000000000000000', 'safeClamp has non-zero FNV-1a proofHash');

    const eqCert = bro.prove(
        function refDouble(x) { return x + x; },
        {
            equivalentTo: function newDouble(x) { return x * 2; },
            domainMin: -100,
            domainMax: 100
        }
    );
    assert(eqCert.equivalent === true, 'bro.prove verifies equivalence of x+x and x*2');
}

// ---- #137: bro.layout.solve (Non-convex symbolic UI & 3D layout solver) -----

{
    const predLayout = bro.layout.solve(
        ['x1', 'w1', 'x2', 'w2'],
        (x1, w1, x2, w2) =>
            x1 === 0 && w1 >= 100 && x2 >= x1 + w1 + 16 && w2 === 2 * w1 && x2 + w2 <= 400,
        { min: 0, max: 500 }
    );
    assert(predLayout !== null && predLayout.sat === true, 'bro.layout.solve predicate form is SAT');
    assert(predLayout.x1 === 0 && predLayout.w1 >= 100, 'predicate layout w1 >= 100');
    assert(predLayout.x2 >= predLayout.x1 + predLayout.w1 + 16, 'predicate layout gap respected');
    assert(predLayout.w2 === 2 * predLayout.w1, 'predicate layout 2:1 width ratio respected');

    const boxLayout = bro.layout.solve({
        container: { w: 300, h: 200 },
        nonOverlap: true,
        gap: 10,
        snapGrid: 10,
        boxes: [
            { id: 'sidebar', minW: 80, maxW: 80, minH: 180, maxH: 180 },
            { id: 'main', minW: 160, maxW: 160, minH: 180, maxH: 180 }
        ],
        constraints: ['sidebar.x == 10', 'sidebar.y == 10', 'main.y == 10']
    });
    assert(boxLayout !== null && boxLayout.sat === true, 'bro.layout.solve declarative box spec is SAT');
    assert(boxLayout.sidebar.x === 10 && boxLayout.sidebar.w === 80, 'sidebar placed at x=10, w=80');
    assert(boxLayout.main.x >= 100 && boxLayout.main.x + boxLayout.main.w <= 300,
           'main box placed without overlap inside container; got x=' + boxLayout.main.x);
}
