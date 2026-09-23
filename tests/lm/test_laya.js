// Weights-gated test for bro.lm.loadLaya: the Laya decision model behind its
// request scheduler, driven the way an app drives it.
//
// Skips (passes with a message) when bro.lm is the stub, the checkpoint is
// absent (LAYA_MODEL_DIR, else the ../laya sibling of the bro checkout) or
// there is no GPU backend. Covers: a blocking predict, many predictAsync
// calls in flight sharing forwards, priority, the promise rejection paths,
// stats(), loadLayaAsync and dispose().
//
// Wall time on an RTX 4090: ~2 s per load (weights + CUDA-graph pre-warm).

const fs = require('node:fs');

const DIR = process.env.LAYA_MODEL_DIR || '../laya';

// advanceTime() runs the frame pump that settles Laya promises; wallSleep()
// gives the device threads wall-clock time (docs/headless.md).
function pumpUntil(desc, fn, seconds) {
    const iters = Math.ceil((seconds * 1000) / 4);
    for (let i = 0; i < iters; i++) {
        advanceTime(16);
        wallSleep(4);
        if (fn()) return;
    }
    throw new Error('timeout waiting for ' + desc);
}

const QUESTIONS = {
    department: {
        type: 'choice', instructions: 'Which department should handle this request?',
        criteria: { billing: 'invoices, payments, refunds', technical: 'bugs, outages, system errors',
                    sales: 'pricing, new contracts', other: 'everything else' },
    },
    urgency: {
        type: 'score', instructions: 'How urgent is this request?',
        criteria: ['not urgent', 'soon', 'critical deadline or blocking issue'],
    },
    churn_risk: { type: 'noul', instructions: 'Does the user threaten to cancel or leave?' },
};

function ticket(i, body) {
    return { id: 'T-' + i, from: 'customer' + i + '@example.com', body };
}

if (bro.lm.available === false) {
    console.log('SKIP: bro.lm is the unavailable stub');
} else if (!fs.existsSync(DIR + '/model.safetensors')) {
    console.log('SKIP: Laya checkpoint not found at ' + DIR);
} else if (!bro.gpu.available) {
    console.log('SKIP: no GPU backend (' + bro.gpu.backend + ')');
} else {
    assert(typeof bro.lm.loadLayaAsync === 'function', 'bro.lm.loadLayaAsync exists');

    // ── Blocking load + predict ──────────────────────────────────────────────
    let t0 = Date.now();
    const laya = bro.lm.loadLaya(DIR, { devices: 'all' });
    console.log('loadLaya: ' + (Date.now() - t0) + ' ms');
    assert(laya instanceof bro.lm.LayaModel, 'handle is a LayaModel');
    const cfg = laya.config();
    assert(cfg.max_len === 512 && cfg.head_max_len === 192, 'checkpoint limits: ' + JSON.stringify(cfg));
    assert(Array.isArray(cfg.devices) && cfg.devices.length >= 1, 'one replica per device: ' + cfg.devices);
    assert(cfg.tokenBudget >= 512 && cfg.tokenBudget <= cfg.maxBatchTokens, 'token budget ' + cfg.tokenBudget);

    const billing = 'We were billed twice for March. Refund the duplicate today or we will cancel our plan.';
    const r = laya.predict(ticket(0, billing), QUESTIONS);
    assert(r.answers.department.choice === 'billing', 'billing ticket routes to billing');
    assert(r.answers.department.confidence > 0.5, 'confident: ' + r.answers.department.confidence);
    assert(r.answers.churn_risk.noul > 0.5, 'churn threat detected: ' + r.answers.churn_risk.noul);
    assert(typeof r.answers.churn_risk.confidence === 'number', 'noul answers carry a confidence');
    assert(r.answers.urgency.legend['2'] === 'critical deadline or blocking issue', 'score legend');
    assert(r.timing.totalMs > 0 && r.timing.forwards >= 1, 'timing: ' + JSON.stringify(r.timing));
    // The act head is saturated; documented, and asserted so a change is noticed.
    assert(r.answers.department.rl_agent.act_probability > 0.99, 'act head saturated');

    // ── Many requests in flight share forwards ───────────────────────────────
    const bodies = [
        billing,
        'The export button returns a 502 and the dashboard is blank since the upgrade.',
        'We are expanding to 600 seats; can we talk about volume pricing?',
        'Thanks, the new invoice layout looks great.',
    ];
    laya.resetStats();
    const want = ['billing', 'technical', 'sales'];
    const got = [];
    let failed = null;
    const N = 48;
    for (let i = 0; i < N; ++i) {
        laya.predictAsync(ticket(i, bodies[i % 4]), QUESTIONS, { priority: i % 2, deadlineMs: 100 })
            .then((res) => got.push({ i, res }), (e) => { failed = e; });
    }
    pumpUntil('48 predictAsync results', () => failed || got.length === N, 60);
    assert(!failed, 'no request failed: ' + failed);
    let shared = 0;
    for (const { i, res } of got) {
        if (i % 4 < 3) {
            assert(res.answers.department.choice === want[i % 4],
                   'ticket ' + i + ' -> ' + res.answers.department.choice);
        }
        if (res.timing.batchRequests > 1) shared++;
    }
    const s = laya.stats();
    console.log('async: ' + shared + '/' + N + ' shared a forward; ' + s.forwards + ' forwards, mean ' +
                s.meanBatchRequests.toFixed(1) + ' requests / ' + s.meanBatchTokens.toFixed(0) +
                ' tokens per forward; p50 ' + s.latencyP50.toFixed(1) + ' ms, p99 ' + s.latencyP99.toFixed(1) + ' ms');
    assert(shared > N / 2, 'concurrent requests batch together');
    assert(s.completed === N && s.failed === 0 && s.inFlightRequests === 0, 'stats counts: ' + JSON.stringify(s));
    assert(s.forwards < N, 'fewer forwards than requests');
    assert(s.recentBatches.length === s.forwards, 'recent forwards recorded');
    assert(s.devices.length === cfg.devices.length, 'per-device stats');

    // ── Rejections: never a throw from predictAsync ─────────────────────────
    const many = [];
    for (let i = 0; i < 200; ++i) many.push('option ' + i);
    let rejected = null, badArgs = null;
    laya.predictAsync('x', { huge: { type: 'choice', instructions: 'pick', criteria: many } })
        .then(() => { rejected = 'resolved'; }, (e) => { rejected = String(e.message); });
    laya.predictAsync(42, QUESTIONS).then(() => { badArgs = 'resolved'; }, (e) => { badArgs = String(e.message); });
    pumpUntil('rejections', () => rejected !== null && badArgs !== null, 10);
    assert(rejected.includes('options do not fit'), 'options that do not fit reject: ' + rejected);
    assert(badArgs.includes('state must be'), 'a bad state rejects: ' + badArgs);

    // ── Async load; dispose frees and fences ─────────────────────────────────
    let second = null, loadErr = null;
    bro.lm.loadLayaAsync({ path: DIR, prewarm: false }).then((m) => { second = m; }, (e) => { loadErr = e; });
    pumpUntil('loadLayaAsync', () => second || loadErr, 60);
    assert(!loadErr, 'loadLayaAsync: ' + loadErr);
    assert(second.predict(ticket(1, bodies[1]), QUESTIONS).answers.department.choice === 'technical',
           'async-loaded model answers');
    second.dispose();
    let threw = '';
    try { second.predict('x', QUESTIONS); } catch (e) { threw = String(e.message); }
    assert(threw.includes('disposed'), 'predict after dispose throws: ' + threw);

    let missing = null;
    bro.lm.loadLayaAsync('tests/lm/__no_such_dir__').then(() => { missing = 'resolved'; },
                                                          (e) => { missing = String(e.message); });
    pumpUntil('missing-path rejection', () => missing !== null, 5);
    assert(missing.includes('no Laya checkpoint'), 'missing path rejects: ' + missing);

    laya.dispose();
    console.log('bro.lm Laya OK');
}
