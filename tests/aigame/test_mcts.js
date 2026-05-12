// MCTS variants — they should return actions in the legal set.

const G = bro.ai.game;

function makeWorld() {
    const w = G.createWorld();
    const h = G.createAgent({ id: 1, teamId: 0, hp: 100, damage: 10, attackRange: 3,
                              x: 0, z: 0, speed: 6 });
    const e = G.createAgent({ id: 2, teamId: 1, hp: 100, damage: 10, attackRange: 3,
                              x: 2, z: 0, speed: 6 });
    w.addAgent(h);
    w.addAgent(e);
    return { w, h, e };
}

function isCombatAction(a) {
    return a && typeof a === 'object' &&
           'moveDir' in a && 'attackSlot' in a && 'abilitySlot' in a &&
           Number.isInteger(a.moveDir) && Number.isInteger(a.attackSlot) && Number.isInteger(a.abilitySlot);
}

// Single-hero MCTS.
{
    const { w, h } = makeWorld();
    const mcts = G.createMcts({
        iterations: 100, rolloutHorizon: 8, simDt: 1/30,
        rolloutPolicy: 'aggressive', opponentPolicy: 'aggressive',
        prior: 'attackBias', evaluator: 'hpDelta', seed: 1234,
    });
    const a = mcts.search(w, h);
    assert(isCombatAction(a), 'single MCTS action shape: ' + JSON.stringify(a));

    // Action must be legal.
    const legals = G.legalActions(h, w);
    assert(Array.isArray(legals) && legals.length > 0, 'legalActions returns nonempty array');
    const found = legals.some(l =>
        l.moveDir === a.moveDir && l.attackSlot === a.attackSlot && l.abilitySlot === a.abilitySlot);
    if (!found) {

        assert(false, 'BUG: MCTS action ' + JSON.stringify(a) + ' not in legal set (len=' + legals.length + ')');
    }

    // lastStats
    const st = mcts.lastStats;
    assert(st && typeof st === 'object', 'mcts.lastStats is object');
    assert(typeof st.iterations === 'number' && st.iterations > 0, 'iterations counted, got ' + st.iterations);

    mcts.advanceRoot(a); // should not throw
}

// Decoupled 1v1.
{
    const { w, h, e } = makeWorld();
    const duel = G.createDecoupledMcts({ iterations: 200, prior: 'attackBias',
                                         evaluator: 'hpDelta', seed: 5678 });
    const joint = duel.search(w, h, e);
    assert(joint && 'hero' in joint && 'opp' in joint, 'decoupled returns {hero,opp}');
    assert(isCombatAction(joint.hero), 'hero action shape');
    assert(isCombatAction(joint.opp), 'opp action shape');
    duel.advanceRoot(joint.hero, joint.opp);
}

// Team MCTS (2v2).
{
    const w = G.createWorld();
    const h1 = G.createAgent({ id: 1, teamId: 0, hp: 100, damage: 10, attackRange: 3, x: -1, z: 0 });
    const h2 = G.createAgent({ id: 2, teamId: 0, hp: 100, damage: 10, attackRange: 3, x:  1, z: 0 });
    const e1 = G.createAgent({ id: 3, teamId: 1, hp: 100, damage: 10, attackRange: 3, x: -1, z: 5 });
    const e2 = G.createAgent({ id: 4, teamId: 1, hp: 100, damage: 10, attackRange: 3, x:  1, z: 5 });
    [h1,h2,e1,e2].forEach(a => w.addAgent(a));

    const team = G.createTeamMcts({ iterations: 200, rolloutHorizon: 8,
                                    rolloutPolicy: 'aggressive', opponentPolicy: 'aggressive',
                                    evaluator: 'teamHpDelta', seed: 4242 });
    const per = team.search(w, [h1, h2]);
    assert(Array.isArray(per), 'team returns array');
    assert(per.length === 2, 'team returns one action per hero, got ' + per.length);
    per.forEach((a, i) => assert(isCombatAction(a), 'team[' + i + '] action shape'));
    team.advanceRoot(per);
}

// OptionMcts macro-actions.
{
    const { w, h } = makeWorld();
    const opt = G.createOption({
        name: "test_hold",
        canInitiate:     () => true,
        step:            () => ({ moveDir: 0, attackSlot: -1, abilitySlot: -1 }),
        shouldTerminate: (_s, _w, ticks) => ticks >= 2,
    });
    const om = G.createOptionMcts({
        iterations: 30, rolloutHorizon: 4, optionMaxWindows: 4,
        options: [opt], opponentPolicy: 'scripted', evaluator: 'hpDelta',
        useLeafValue: true, seed: 99,
    });
    const chosen = om.search(w, h);
    // chosen may be null if no legal option, but with canInitiate=>true it should be "test_hold".
    if (chosen === null) {
        assert(false, 'BUG: OptionMcts returned null despite an always-legal option');
    }
    assert(typeof chosen === 'string', 'OptionMcts returns option name string, got ' + typeof chosen);
    assert(chosen === 'test_hold', 'chosen option name, got ' + chosen);
}

console.log('test_mcts: OK');
