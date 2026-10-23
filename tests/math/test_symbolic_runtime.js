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

// ---- #142: bro.superoptimize (Counterexample-Pruned CEGIS Superoptimizer) ---

{
    assert(typeof bro.superoptimize === 'function', 'bro.superoptimize is a function');
    const sres = bro.superoptimize(`
        function branchyAbs(x) {
            if (x < 0) return -x;
            return x;
        }
    `);
    assert(sres.verified === true && sres.optimized === true, 'bro.superoptimize synthesizes branchless abs');
    assert(sres.synthesizedExpression === '((x ^ (x >> 31)) - (x >> 31))',
           'synthesized Hacker\'s Delight abs; got ' + sres.synthesizedExpression);
    assert(sres.stats.concretePruned > 0, 'concrete pruning filtered candidates');
}

// ---- #143: bro.rewriteSource (Source-to-Source Verified JS Rewriter) --------

{
    assert(typeof bro.rewriteSource === 'function', 'bro.rewriteSource is a function');
    const rep = bro.rewriteSource(`
        function polySoup(x) {
            let u = x & 255;
            if (u < 0) return -1;
            let a = Math.max(0, x & 255);
            let b = Math.floor((x & 255) / 8);
            let p = Math.pow(u, 3) + 3 * Math.pow(u, 2) + 3 * u + 1;
            return a + b + p;
        }
    `);
    assert(rep.allProvenEquivalent === true, 'bro.rewriteSource proves equivalence');
    assert(rep.rewrites.length >= 3, 'bro.rewriteSource applies multiple passes');
    assert(rep.estimatedOpsAfter < rep.estimatedOpsBefore, 'ops reduced');
}

// ---- #144: bro.autoKernel (SMT Loop-to-@kernel Parallelizer) ----------------

{
    assert(typeof bro.autoKernel === 'function', 'bro.autoKernel is a function');
    const okLoop = bro.autoKernel(`
        function saxpy(out, x, y, a, n) {
            for (let i = 0; i < n; i++) {
                out[i] = a * x[i] + y[i];
            }
        }
    `);
    assert(okLoop.parallelizable === true, 'saxpy loop proven parallelizable');
    assert(okLoop.provenNoWaw && okLoop.provenNoRaw && okLoop.provenNoWar, 'no loop-carried hazards');
    assert(okLoop.kernel !== null && typeof okLoop.kernel === 'object', 'compiled @kernel attached');

    const badLoop = bro.autoKernel(`
        function prefixSum(a, x, n) {
            for (let i = 0; i < n; i++) {
                a[i] = a[i - 1] + x[i];
            }
        }
    `);
    assert(badLoop.parallelizable === false, 'prefixSum loop rejected due to RAW dependency');
    assert(badLoop.hazards.length >= 1 && badLoop.hazards[0].hazardKind === 'RAW',
           'reports concrete RAW hazard witness');
}

// ---- #148: bro.live (Self-Healing Proof-Gated Live-Reload Supervisor) -------

{
    assert(typeof bro.live === 'object' && typeof bro.live.healAndVerify === 'function',
           'bro.live.healAndVerify is a function');
    assert(typeof bro.live.createSupervisor === 'function',
           'bro.live.createSupervisor is a function');

    const healed = bro.live.healAndVerify(`
        function riskyDsp(x, y, sh) {
            let s = Math.sqrt(x - y);
            let a = Math.acos(x);
            let l = Math.log(y);
            let d = 100 / y;
            let m = 1 << sh;
            return s + a + l + d + m;
        }
    `);
    assert(healed.accepted === true && healed.selfHealed === true,
           'bro.live.healAndVerify automatically repairs domain/division/shift hazards');
    assert(healed.originalHazardCount >= 4 && healed.remainingHazardCount === 0,
           'all detected hazards healed to 0 remaining');
    assert(typeof healed.fn === 'function', 'healed.fn is callable');

    const sup = bro.live.createSupervisor('function step(x) { return x + x; }');
    assert(sup.call(21) === 42, 'initial supervisor call(21) === 42');

    const r1 = sup.reload('function step(x) { return x * 2; }');
    assert(r1.accepted === true && r1.behaviorallyIdentical === true,
           'pure refactor proven behaviorally identical');
    assert(r1.divergenceWitness === null, 'no divergence witness on pure refactor');

    const r2 = sup.reload('function step(x) { return x * 2 + 1; }');
    assert(r2.accepted === true && r2.behaviorallyIdentical === false,
           'semantic change detected by SMT equivalence prover');
    assert(r2.divergenceWitness !== null && r2.divergenceWitness.oldOutput !== r2.divergenceWitness.newOutput,
           'divergence witness reports concrete input where outputs differ');
    assert(sup.call(21) === 43, 'reloaded supervisor call(21) === 43');
}

// ---- #149: bro.evolve & bro.daemon.createEvolutionEngine --------------------

{
    assert(typeof bro.evolve === 'function', 'bro.evolve is a function');
    assert(typeof bro.daemon === 'object' && typeof bro.daemon.createEvolutionEngine === 'function',
           'bro.daemon.createEvolutionEngine is a function');

    const evolvedAbs = bro.evolve(`
        function branchyAbs(x) {
            if (x < 0) return -x;
            return x;
        }
    `);
    assert(typeof evolvedAbs === 'function', 'bro.evolve returns a callable function');
    assert(evolvedAbs.provenEquivalent === true, 'evolvedAbs is proven equivalent');
    assert(Array.isArray(evolvedAbs.generations) && evolvedAbs.generations.length >= 2,
           'evolvedAbs tracks multi-generation lineage');
    assert(evolvedAbs.bestTier === 'cegis-superopt',
           'evolvedAbs reaches cegis-superopt tier; got ' + evolvedAbs.bestTier);
    assert(evolvedAbs.speedup > 1.0, 'evolvedAbs achieves >1.0x speedup');
    assert(evolvedAbs(-42) === 42 && evolvedAbs(19) === 19,
           'evolvedAbs executes accurately');

    const eng = bro.daemon.createEvolutionEngine();
    eng.register('absKernel', `
        function absKernel(x) {
            if (x < 0) return -x;
            return x;
        }
    `);
    eng.register('saxpyLoop', `
        function saxpyLoop(out, x, y, a, n) {
            for (let i = 0; i < n; i++) {
                out[i] = a * x[i] + y[i];
            }
        }
    `);
    const engRep = eng.tick();
    assert(engRep.totalFunctions === 2 && engRep.evolvedCount === 2,
           'evolution engine evolved both registered functions');
    assert(engRep.averageSpeedup > 1.0, 'evolution engine averageSpeedup > 1.0');
}

// ---- #152: Proof-to-Codegen Unlock (TypedArray integral-index & in-bounds) --

{
    const cert = bro.prove(`
        function typedLoopKernel() {
            const arr = new Float64Array(8);
            let acc = 0;
            for (let i = 0; i < arr.length; i++) {
                arr[i] = i * 2 + 1;
                acc += arr[i];
            }
            return acc;
        }
    `);
    assert(cert.verified === true && cert.proofsValid === true,
           'bro.prove verifies TypedArray loop with integral-index and in-bounds proofs');
    assert(cert.provenGuards >= 6,
           'bro.prove reflects receiver + integral-index + in-bounds proven guards; got ' + cert.provenGuards);

    const a = new Float64Array(8);
    let sum = 0;
    for (let i = 0; i < a.length; i++) {
        a[i] = i * 2 + 1;
        sum += a[i];
    }
    assert(sum === 64, 'zero-check TypedArray loop executes accurately; got ' + sum);
}

