// root_parallel_search / root_parallel_search_decoupled — N native threads
// over independent Worlds, joined and merged into one action. CPU-only
// (string-preset evaluator/rollout — no GPU/neural path needed here).

const G = bro.ai.game;

function makeWorld() {
    const w = G.createWorld();
    const h = G.createAgent({ id: 1, teamId: 0, hp: 100, damage: 10, attackRange: 3,
                              x: 0, z: 0, speed: 6 });
    const e = G.createAgent({ id: 2, teamId: 1, hp: 100, damage: 10, attackRange: 3,
                              x: 2, z: 0, speed: 6 });
    w.addAgent(h);
    w.addAgent(e);
    return w;
}

function isCombatAction(a) {
    return a && typeof a === 'object' &&
           'moveDir' in a && 'attackSlot' in a && 'abilitySlot' in a &&
           Number.isInteger(a.moveDir) && Number.isInteger(a.attackSlot) && Number.isInteger(a.abilitySlot);
}

// Single-player root-parallel search across 4 independent worlds.
{
    const worlds = [makeWorld(), makeWorld(), makeWorld(), makeWorld()];
    const { action, stats } = G.rootParallelSearch({
        worlds, heroId: 1,
        iterations: 60, rolloutHorizon: 8, simDt: 1 / 30,
        rolloutPolicy: 'aggressive', opponentPolicy: 'aggressive', evaluator: 'hpDelta',
        seed: 1234,
    });
    assert(isCombatAction(action), 'rootParallelSearch action shape: ' + JSON.stringify(action));
    assert(stats && typeof stats === 'object', 'rootParallelSearch returns stats object');
    assert(stats.numThreads === 4, 'stats.numThreads === 4, got ' + stats.numThreads);
    assert(stats.totalIterations > 0, 'stats.totalIterations > 0, got ' + stats.totalIterations);
    assert(stats.mergedBestVisits > 0, 'stats.mergedBestVisits > 0, got ' + stats.mergedBestVisits);
}

// Decoupled (simultaneous-move) root-parallel search.
{
    const worlds = [makeWorld(), makeWorld(), makeWorld()];
    const { hero, opp, stats } = G.rootParallelSearchDecoupled({
        worlds, heroId: 1, oppId: 2,
        iterations: 60, rolloutHorizon: 8, simDt: 1 / 30,
        rolloutPolicy: 'aggressive', evaluator: 'hpDelta', seed: 99,
    });
    assert(isCombatAction(hero), 'decoupled hero action shape: ' + JSON.stringify(hero));
    assert(isCombatAction(opp), 'decoupled opp action shape: ' + JSON.stringify(opp));
    assert(stats.numThreads === 3, 'decoupled stats.numThreads === 3, got ' + stats.numThreads);
}

// Empty/missing worlds must throw, not silently no-op.
{
    let threw = false;
    try { G.rootParallelSearch({ worlds: [], heroId: 1 }); }
    catch (e) { threw = true; }
    assert(threw, 'BUG: rootParallelSearch with empty worlds array should throw');
}

// A JS-function evaluator must be rejected — worker threads cannot safely
// call back into QuickJS.
{
    const worlds = [makeWorld(), makeWorld()];
    let threw = false;
    try {
        G.rootParallelSearch({
            worlds, heroId: 1, iterations: 20,
            evaluator: function (worldView, heroId) { return 0; },
        });
    } catch (e) { threw = true; }
    assert(threw, 'BUG: rootParallelSearch with a JS-function evaluator should throw TypeError');
}

console.log('test_root_parallel: OK');
