// Tests for apps/lib/math.js.
//
// Run: bro-headless apps/lib-tests apps/lib-tests/test_math.js

'use strict';

let tests = 0, failed = 0;
function t(name, fn) {
    tests++;
    try { fn(); console.log('  ok   ' + name); }
    catch (e) {
        failed++;
        console.log('  FAIL ' + name + ': ' + (e && e.message ? e.message : e));
        if (e && e.stack) console.log(e.stack);
    }
}
function eq(a, b, msg) {
    const ja = JSON.stringify(a), jb = JSON.stringify(b);
    if (ja !== jb) throw new Error((msg || 'eq') + ': ' + ja + ' !== ' + jb);
}
function near(a, b, eps, msg) {
    eps = eps || 1e-9;
    if (Math.abs(a - b) > eps) throw new Error((msg || 'near') + ': ' + a + ' !== ' + b);
}
function truthy(v, msg) { if (!v) throw new Error(msg || 'expected truthy'); }

// ---------- clamp ----------

t('clamp inside range is identity', () => {
    eq(MathX.clamp(5, 0, 10), 5);
});
t('clamp below lo returns lo', () => {
    eq(MathX.clamp(-1, 0, 10), 0);
});
t('clamp above hi returns hi', () => {
    eq(MathX.clamp(99, 0, 10), 10);
});
t('clamp at boundaries is exact', () => {
    eq(MathX.clamp(0, 0, 10), 0);
    eq(MathX.clamp(10, 0, 10), 10);
});

// ---------- lerp ----------

t('lerp endpoints', () => {
    eq(MathX.lerp(0, 10, 0), 0);
    eq(MathX.lerp(0, 10, 1), 10);
});
t('lerp midpoint', () => {
    eq(MathX.lerp(0, 10, 0.5), 5);
});
t('lerp extrapolates', () => {
    eq(MathX.lerp(0, 10, 2), 20);
    eq(MathX.lerp(0, 10, -1), -10);
});

// ---------- angleNorm ----------

t('angleNorm leaves in-range unchanged', () => {
    near(MathX.angleNorm(0), 0);
    near(MathX.angleNorm(1), 1);
    near(MathX.angleNorm(-1), -1);
});
t('angleNorm wraps positive overshoot', () => {
    near(MathX.angleNorm(MathX.PI + 0.1), -MathX.PI + 0.1);
});
t('angleNorm wraps negative overshoot', () => {
    near(MathX.angleNorm(-MathX.PI - 0.1), MathX.PI - 0.1);
});
t('angleNorm wraps multi-turn input', () => {
    near(MathX.angleNorm(MathX.TAU * 3 + 0.5), 0.5, 1e-9);
});

// ---------- lerpAngle ----------

t('lerpAngle takes the short way across PI', () => {
    // From 3.0 to -3.0 — short way is +0.28 (across PI), not -6.0.
    const r = MathX.lerpAngle(3.0, -3.0, 0.5);
    truthy(Math.abs(r) > MathX.PI - 0.2 || Math.abs(Math.abs(r) - MathX.PI) < 0.2,
        'expected near PI/-PI, got ' + r);
});
t('lerpAngle endpoints', () => {
    near(MathX.lerpAngle(0.5, 1.0, 0), 0.5);
    near(MathX.lerpAngle(0.5, 1.0, 1), 1.0);
});

// ---------- randRange ----------

t('randRange stays in [lo, hi)', () => {
    for (let i = 0; i < 1000; i++) {
        const v = MathX.randRange(5, 10);
        truthy(v >= 5 && v < 10, 'out of range: ' + v);
    }
});

// ---------- randInt ----------

t('randInt covers both ends inclusively', () => {
    let sawLo = false, sawHi = false;
    for (let i = 0; i < 2000; i++) {
        const v = MathX.randInt(3, 5);
        truthy(v === 3 || v === 4 || v === 5, 'unexpected: ' + v);
        truthy(Number.isInteger(v), 'not int: ' + v);
        if (v === 3) sawLo = true;
        if (v === 5) sawHi = true;
    }
    truthy(sawLo, 'never hit low end');
    truthy(sawHi, 'never hit high end');
});

t('randInt with lo===hi is constant', () => {
    for (let i = 0; i < 50; i++) eq(MathX.randInt(7, 7), 7);
});

// ---------- randPick ----------

t('randPick returns elements only from arr', () => {
    const arr = ['a', 'b', 'c'];
    for (let i = 0; i < 200; i++) {
        truthy(arr.indexOf(MathX.randPick(arr)) >= 0);
    }
});

t('randPick single-element array', () => {
    eq(MathX.randPick(['only']), 'only');
});

// ---------- vecFromAngle ----------

t('vecFromAngle at 0 is +x unit', () => {
    const v = MathX.vecFromAngle(0);
    near(v.x, 1); near(v.y, 0);
});
t('vecFromAngle at PI/2 is +y unit', () => {
    const v = MathX.vecFromAngle(MathX.PI / 2);
    near(v.x, 0, 1e-9); near(v.y, 1);
});
t('vecFromAngle scales by mag', () => {
    const v = MathX.vecFromAngle(0, 5);
    near(v.x, 5); near(v.y, 0);
});
t('vecFromAngle default mag is 1', () => {
    const v = MathX.vecFromAngle(MathX.PI);
    near(Math.hypot(v.x, v.y), 1);
});

// ---------- dist2 ----------

t('dist2 zero when same point', () => {
    eq(MathX.dist2(3, 4, 3, 4), 0);
});
t('dist2 squared distance', () => {
    eq(MathX.dist2(0, 0, 3, 4), 25);
});

// ---------- end ----------

console.log(`\n${tests - failed}/${tests} passed`);
if (failed > 0) throw new Error(`${failed} test(s) failed`);
