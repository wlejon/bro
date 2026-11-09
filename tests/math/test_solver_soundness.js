// The verifier globals must never return a wrong answer: what the model does
// not have is refused by name, and the rest is answered at full fidelity.
// Every case here once came back wrong without an error (docs/solve-api.js).

function throwsWith(fn, part, label) {
    let msg = null;
    try { fn(); } catch (e) { msg = String(e && e.message); }
    assert(msg !== null, label + ': expected a throw');
    assert(msg.indexOf(part) >= 0, label + ': message "' + msg + '" lacks "' + part + '"');
}

// ---- string operations on int32 parameters ---------------------------------
{
    throwsWith(() => bro.solve((x) => String(x).length === 2),
               'function not symbolically decidable: string conversion of a number', 'String(x)');
    throwsWith(() => bro.solveWithCert((x) => String(x).length === 1 && x > 9, { min: 0, max: 100 }),
               'string conversion of a number', 'String(x) bounded');
    throwsWith(() => bro.solve((x) => `${x}`.length === 2), 'string conversion of a number', 'template');
    throwsWith(() => bro.solve((x) => ('a' + x).length === 2), 'string concatenation with a number', 'concat');
    throwsWith(() => bro.solve((x) => x.length === 3), 'parameter x is read as a string, array or object',
               'x.length');
    const lit = bro.solve((x) => ('ab' + 'c').length === x);
    assert(lit !== null && lit.x === 3, 'string literals stay modelled; got ' + (lit && lit.x));
}

// ---- free variables ---------------------------------------------------------
{
    const k = 4;
    throwsWith(() => bro.solveWithCert((x) => x === k),
               "free variable k: the function's source is analysed, not its closure", 'solve k');
    const j = 3;
    throwsWith(() => bro.prove((x) => x + j, { equivTo: (x) => x + 3 }), 'free variable j', 'prove j');
    throwsWith(() => bro.prove((x) => x + 3, { equivTo: (x) => x + j }), 'equivTo function: free variable j',
               'prove equivTo j');
    throwsWith(() => bro.fuzz((x) => x > k ? 1 : 0), 'free variable k', 'fuzz k');
    const o = bro.optimize((x) => x * k);
    assert(o.verified === false && o.summary.indexOf('free variable k') >= 0, 'optimize k: ' + o.summary);
    const s = bro.superoptimize((x) => x * k);
    assert(s.verified === false && s.summary.indexOf('free variable k') >= 0, 'superoptimize k: ' + s.summary);
    // Known globals are not free variables.
    assert(bro.solve((x) => Math.abs(x) === 3 && x < 0).x === -3, 'Math is a known global');
}

// ---- target width ------------------------------------------------------------
{
    const u = bro.solve((x) => x >>> 0, { target: 4294967295 });
    assert(u !== null && u.x === -1 && u.returnValue === 4294967295,
           'x >>> 0 reaches 2^32-1 at x = -1; got ' + JSON.stringify(u && [...u]));
    assert(bro.solve((x) => x, { target: 3000000000 }) === null, 'no int32 x equals 3e9');
    assert(bro.solve((x) => x * 2, { target: 3000000000 }).x === 1500000000, 'x * 2 reaches 3e9');
    throwsWith(() => bro.solve((x) => x, { target: 1.5 }), 'target must be', 'fractional target');
    throwsWith(() => bro.solve((x) => x, { target: 2 ** 60 }), 'target must be', 'huge target');
    assert(bro.solve((x) => x > 5, { target: true, min: 0, max: 6 }).x === 6, 'boolean target');
}

// ---- layout predicate binds parameters by name -----------------------------
{
    const spec = { container: { w: 100, h: 50 }, boxes: { a: { w: 20, h: 20 }, b: { w: 20, h: 20 } } };
    const L = bro.layout.solve(spec, (a_x, b_x) => b_x === a_x + 30);
    assert(L !== null && L.b.x === L.a.x + 30, 'b.x = a.x + 30; got ' + JSON.stringify(L && [L.a, L.b]));
    const R = bro.layout.solve(spec, (b_y, a_x) => b_y === 25 && a_x === 7);
    assert(R !== null && R.b.y === 25 && R.a.x === 7, 'order of parameters does not matter');
    throwsWith(() => bro.layout.solve(spec, (a_x, c_x) => c_x === a_x),
               "predicate parameter 'c_x' names no box variable", 'unknown box');
}

// ---- the optimize callable runs the proven code only on its domain ----------
{
    const len = bro.optimize((s) => s.length);
    assert(len('abcd') === 4, 's.length runs the original; got ' + len('abcd'));
    const gt = bro.optimize((x) => x > 3);
    assert(gt(5) === true && gt(1) === false, 'a boolean function returns booleans; got ' + gt(5));
    const abs = bro.optimize((x) => (x < 0 ? -x : x));
    assert(abs.optimized === true, 'abs optimizes: ' + abs.summary);
    assert(abs(-7) === 7, 'in the domain');
    assert(abs(-2147483648) === 2147483648, 'outside the domain runs the original; got ' + abs(-2147483648));
    assert(abs(-2.5) === 2.5, 'a non-integer runs the original; got ' + abs(-2.5));
    const half = bro.optimize((x) => x * 8 | 0);
    assert(half(5) === 40, 'optimized x * 8 | 0');
    assert(half('5') === 40, 'a string argument runs the original');
    const maybe = bro.optimize((x) => { if (x > 0) return 1; });
    assert(maybe(-1) === undefined, 'a missing return is undefined; got ' + maybe(-1));
}

// ---- prove: the result kind is part of equivalence --------------------------
{
    const p = bro.prove((x) => x > 3, { equivTo: (x) => (x > 3 ? 1 : 0), synthesizeTests: false });
    assert(p.equivalent === false && p.verified === false, 'boolean vs number is not equivalent');
    assert(p.equivalenceDetail.indexOf('boolean') >= 0, 'detail names the kinds: ' + p.equivalenceDetail);
    const q = bro.prove((x) => x > 3, { equivTo: (x) => !(x <= 3), synthesizeTests: false });
    assert(q.equivalent === true, 'two boolean forms stay equivalent: ' + q.equivalenceDetail);
}

// ---- prove: sha256 is SHA-256 -------------------------------------------------
{
    // SHA-256 of the normalized source 'export function __prove_target(x) { return (x); }'.
    const h = bro.prove((x) => x, { synthesizeTests: false });
    assert(h.sha256 === '507fcda25982aa10bd6d53813e42c0f47f54faa42b6582adae883567896e658a',
           'sha256 of the normalized source; got ' + h.sha256);
    assert(h.proofHash === h.sha256, 'proofHash is the same digest');
}
