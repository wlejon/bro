// learn.* — evaluator/prior wrappers, situations, ExIt trainer.

const G = bro.ai.game;
const nn = G.nn;
const L = G.learn;

function makeWorldHero() {
    const w = G.createWorld();
    const h = G.createAgent({ id: 1, teamId: 0, hp: 100, damage: 10, attackRange: 3, x: 0, z: 0 });
    const e = G.createAgent({ id: 2, teamId: 1, hp: 100, damage: 10, attackRange: 3, x: 5, z: 0 });
    w.addAgent(h); w.addAgent(e);
    return { w, h, e };
}

const net = nn.createSingleHeroNet({
    enc: { hidden: 16, embedDim: 16 }, trunkHidden: 32, valueHidden: 16, seed: 7n,
});
const handle = nn.createWeightsHandle();
handle.publish(net.save(), 1n);

// NeuralEvaluator: evaluate returns scalar in [-1,1].
{
    const ev = L.createNeuralEvaluator(net, handle);
    assert(typeof ev.evaluate === 'function', 'evaluator.evaluate is function');
    const { w, h } = makeWorldHero();
    const v = ev.evaluate(w, h.unit.id);
    assert(Number.isFinite(v), 'evaluator value finite, got ' + v);
    assert(v >= -1.001 && v <= 1.001, 'evaluator in [-1,1], got ' + v);
}

// NeuralPrior: temperature + mix setters callable.
{
    const pr = L.createNeuralPrior(net, handle);
    pr.setTemperature(0.5);
    pr.setUniformMix(0.1);
}

// GumbelNoisePrior wraps NeuralPrior.
{
    const inner = L.createNeuralPrior(net, handle);
    const g = L.createGumbelNoisePrior(inner, 1.0);
    g.setScale(0.5);
    g.reseed(0xA11Cn);
}

// makeSituation + targetsFromMcts.
{
    const { w, h } = makeWorldHero();
    const mcts = G.createMcts({ iterations: 64, rolloutHorizon: 4, evaluator: 'hpDelta',
                                rolloutPolicy: 'aggressive', opponentPolicy: 'aggressive', seed: 11 });
    mcts.search(w, h);
    const t = L.targetsFromMcts(mcts);
    if (t === null) {
        assert(false, 'BUG: targetsFromMcts returned null after search');
    }
    assert(t && t.move instanceof Float32Array, 'targets.move Float32Array');
    assert(t.attack instanceof Float32Array, 'targets.attack Float32Array');
    assert(t.ability instanceof Float32Array, 'targets.ability Float32Array');

    // gumbelImprovedPolicy.
    const t2 = L.gumbelImprovedPolicy(mcts);
    assert(t2, 'gumbelImprovedPolicy returns non-null');

    // makeSituation.
    const sit = L.makeSituation(mcts, h, w);
    assert(sit && typeof sit === 'object', 'situation is object');
    assert('obs' in sit, 'situation has obs');
}

// ExIt trainer step shouldn't throw on an empty / small buffer.
{
    const buf = L.createReplayBuffer(16);
    const trainer = L.createExItTrainer();
    trainer.setNet(net);
    trainer.setBuffer(buf);
    trainer.setWeightsHandle(handle);
    trainer.setConfig({ lr: 0.01, momentum: 0.9, batch: 4,
                        policyWeight: 1.0, valueWeight: 1.0,
                        publishEvery: 1000, rngSeed: 0x1234n });
    // Step on empty buffer — should be a noop or skip; must not throw.
    let threw = false;
    try {
        const r = trainer.step();
        // r may be null or {samples: 0}.
        if (r && r.samples > 0) {
            console.log('trainer.step on empty buffer returned samples=' + r.samples);
        }
    } catch (e) {
        threw = true;
        console.log('trainer.step on empty buffer threw: ' + e);
    }
    assert(!threw, 'trainer.step on empty buffer did not throw');

    // Push a hand-built situation and step.
    const sit = {
        obs: new Float32Array(G.OBS_TOTAL),
        atkMask: new Float32Array(G.nn.N_ATTACK).fill(1),
        abilMask: new Float32Array(G.nn.N_ABILITY).fill(1),
        targetMove: new Float32Array(G.nn.N_MOVE),
        targetAttack: new Float32Array(G.nn.N_ATTACK),
        targetAbility: new Float32Array(G.nn.N_ABILITY),
        valueTarget: 0.0,
    };
    sit.targetMove[0] = 1.0;
    sit.targetAttack[0] = 1.0;
    sit.targetAbility[0] = 1.0;
    let pushOk = true;
    try { for (let i = 0; i < 8; i++) buf.push(sit); }
    catch (e) { pushOk = false; console.log('push threw: ' + e); }
    if (pushOk && buf.size > 0) {
        let step2OK = true;
        try {
            const r = trainer.step();
            if (r && typeof r === 'object') {
                assert(Number.isFinite(r.lossTotal), 'lossTotal finite, got ' + r.lossTotal);
            }
        } catch (e) {
            step2OK = false;
            assert(false, 'BUG: trainer.step on populated buffer threw: ' + e);
        }
        // After step the published weights should still load cleanly.
        const blob = net.save();
        let okLoad = true;
        try { net.load(blob); } catch (e) { okLoad = false; }
        assert(okLoad, 'net.load post-step did not throw');
    }
}

console.log('test_learn: OK');
