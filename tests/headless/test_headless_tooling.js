// Verifies advanceRealtime(steps, opts) pacing and control pumping in bro-headless.
let ticks = 0;
setInterval(() => { ticks++; }, 25);

const res = advanceRealtime(4, { targetHz: 100, maxDtMs: 40 });
assert(res && res.steps === 4, 'advanceRealtime ran 4 steps: ' + JSON.stringify(res));
assert(res.simMs >= 4 && res.simMs <= 160, 'simMs bounded by steps * maxDtMs: ' + res.simMs);
assert(res.wallMs >= 20, 'wallMs honored targetHz spacing: ' + res.wallMs);

const cid = controlCommand('eval', '21 * 2');
let reply = null;
for (let i = 0; i < 20 && !reply; i++) {
    advanceRealtime(1, { minWallMs: 2, maxDtMs: 25 });
    reply = controlResult(cid);
}
assert(reply && reply.ok && reply.payload === '42', 'controlCommand answered during advanceRealtime: ' + JSON.stringify(reply));
assert(ticks >= 1, 'virtual timers advanced during advanceRealtime');
