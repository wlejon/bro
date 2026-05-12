// GenericMcts on a trivial deterministic 2-state, 2-action env.
//
// State 0: action 0 -> state 1 (reward 1, done); action 1 -> state 0 (reward 0, not done).
// Should always prefer action 0.

const G = bro.ai.game;

let state = 0;
const env = {
    numActions: 2,
    snapshot() { return state; },
    restore(s) { state = s; },
    step(a) {
        if (state === 0 && a === 0) { state = 1; return { reward: 1, done: true }; }
        if (state === 0 && a === 1) { state = 0; return { reward: 0, done: false }; }
        return { reward: 0, done: true };
    },
    legalActions() { return state === 0 ? [0, 1] : []; },
    observe() { return new Float32Array([state]); },
};

const m = G.createGenericMcts({
    env,
    iterations: 200,
    cPuct: 1.5,
    gamma: 0.99,
    rolloutDepth: 4,
    seed: 0xC0DE1234n,
});

state = 0;
const action = m.search();
assert(typeof action === 'number', 'GenericMcts search returns number, got ' + typeof action);
assert(action === 0, 'GenericMcts picks rewarding action 0, got ' + action);

const visits = m.rootVisits();
assert(visits instanceof Float32Array, 'rootVisits is Float32Array');
assert(visits.length === 2, 'rootVisits length = numActions, got ' + visits.length);
const sum = visits[0] + visits[1];
assert(Math.abs(sum - 1) < 0.01 || sum === 0, 'rootVisits sums to ~1, got ' + sum);
assert(visits[0] > visits[1], 'action 0 visited more, got [' + visits[0] + ',' + visits[1] + ']');

const stats = m.lastStats();
assert(stats && typeof stats === 'object', 'lastStats is object');
assert(stats.iterations >= 1, 'lastStats.iterations >=1, got ' + stats.iterations);
assert(stats.bestAction === 0, 'lastStats.bestAction = 0, got ' + stats.bestAction);

// Empty legal: docs say search should return -1 when "action space is empty".
state = 1;
m.reset();
const noop = m.search();
// BUG: generic_mcts.search() only checks env.num_actions, not legalActions().
// When legal set is empty but numActions > 0, it returns the default best
// action (0) instead of -1 as documented.
if (noop !== -1) {
    console.log('BUG: GenericMcts.search returned ' + noop + ' for empty legal set (docs say -1)');
}
assert(noop === -1, 'BUG: search returns -1 on empty action space, got ' + noop);

// reconfigure
state = 0;
m.reset();
m.setConfig({ iterations: 50 });
const a2 = m.search();
assert(a2 === 0, 'still picks 0 after reconfig, got ' + a2);

console.log('test_generic_mcts: OK');
