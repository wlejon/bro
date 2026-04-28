// mcts_js.js — generic PUCT MCTS over a JS-side environment.
//
// The environment interface is duck-typed:
//   env.snapshot()                 -> opaque state object (deep-copied internally)
//   env.restore(state)             -> restore the env to `state`
//   env.legalActions()             -> Int32Array | Array<int> of legal action indices
//   env.step(action)               -> { reward: number, done: boolean }
//                                     (mutates the env; reward is the per-decision delta)
//   env.observe()                  -> Float32Array (used as net input + cache key)
//   env.numActions                 -> int (max action index + 1; mask shape)
//
// Optional prior + value providers (omit to use uniform prior + random rollout):
//   priorFn(obs, legalActions)     -> Float32Array(numActions) probs over legal actions
//   valueFn(obs)                   -> number in [-1, 1]
//
// Tree nodes are plain objects to keep allocation cheap. Nothing is shared
// between MCTS instances; one per agent. All env interaction routes through
// `env.snapshot/restore/step` — the env owns physics and entity state.
//
// Usage:
//   const mcts = MctsJs.create({ env, cPuct: 1.5, numActions: 6 });
//   const action = mcts.search({
//       iterations: 200,
//       priorFn(obs, legal) { return netForwardProbs(obs, legal); },
//       valueFn(obs)        { return netForwardValue(obs); },
//       rolloutDepth: 8,         // used when valueFn omitted (random rollout)
//       gamma: 0.99,
//   });
//   const visits = mcts.rootVisits();        // Float32Array(numActions) — policy target
//   mcts.advanceRoot(action);                // (optional) reuse subtree next call

(function (global) {
    'use strict';

    function create(opts) {
        const env = opts.env;
        const numActions = opts.numActions != null ? opts.numActions : env.numActions;
        const cPuct = opts.cPuct != null ? opts.cPuct : 1.5;

        // ── PRNG (Mulberry32-ish) for deterministic rollouts and noise ──────
        let _seed = (opts.seed != null ? opts.seed : 0xC0DE1234) >>> 0;
        function rand() {
            _seed = (_seed + 0x6D2B79F5) >>> 0;
            let t = _seed;
            t = Math.imul(t ^ (t >>> 15), t | 1);
            t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
            return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
        }

        // Box-Muller standard normal off the same PRNG.
        function randn() {
            const u1 = Math.max(1e-12, rand());
            const u2 = rand();
            return Math.sqrt(-2 * Math.log(u1)) * Math.cos(2 * Math.PI * u2);
        }
        // Marsaglia–Tsang Gamma(α, 1) sampler. Handles α<1 via the boost
        // identity: Gamma(α) = Gamma(α+1) * U^(1/α).
        function sampleGamma(alpha) {
            if (alpha < 1) {
                return sampleGamma(alpha + 1) * Math.pow(Math.max(1e-12, rand()), 1 / alpha);
            }
            const d = alpha - 1 / 3;
            const c = 1 / Math.sqrt(9 * d);
            // Tail probability of rejection is tiny; loop is bounded in practice.
            for (;;) {
                let x, v;
                do { x = randn(); v = 1 + c * x; } while (v <= 0);
                v = v * v * v;
                const u = rand();
                if (u < 1 - 0.0331 * x * x * x * x) return d * v;
                if (Math.log(u) < 0.5 * x * x + d * (1 - v + Math.log(v))) return d * v;
            }
        }
        // Symmetric Dirichlet(α) over the legal-action subset; entries on
        // illegal actions stay zero.
        function sampleDirichlet(legal, alpha) {
            const out = new Float32Array(numActions);
            let sum = 0;
            for (let i = 0; i < legal.length; i++) {
                const g = sampleGamma(alpha);
                out[legal[i]] = g;
                sum += g;
            }
            if (sum > 0) {
                for (let i = 0; i < legal.length; i++) out[legal[i]] /= sum;
            } else {
                const u = 1 / Math.max(1, legal.length);
                for (let i = 0; i < legal.length; i++) out[legal[i]] = u;
            }
            return out;
        }

        // ── Tree node ───────────────────────────────────────────────────────
        // n.children[a] is the Node for taking action `a`, or null if not expanded.
        // n.P[a] is the prior, n.N[a] visits, n.W[a] sum of values, n.legal[a] bool.
        function newNode() {
            const P = new Float32Array(numActions);
            const N = new Int32Array(numActions);
            const W = new Float32Array(numActions);
            return {
                children: new Array(numActions).fill(null),
                legal: new Uint8Array(numActions),
                P, N, W,
                visits: 0,    // sum of N[a]
                expanded: false,
                rewardFromParent: 0,  // r(s, a) recorded when this node was expanded
                terminal: false,      // true if env.step returned done at this node
            };
        }

        // ── Selection: PUCT-best legal child of `node` ──────────────────────
        function pickAction(node) {
            const sqrtParent = Math.sqrt(Math.max(1, node.visits));
            let bestA = -1, bestScore = -Infinity;
            for (let a = 0; a < numActions; a++) {
                if (!node.legal[a]) continue;
                const Na = node.N[a];
                const Q  = Na > 0 ? node.W[a] / Na : 0;
                const U  = cPuct * node.P[a] * sqrtParent / (1 + Na);
                const s  = Q + U;
                if (s > bestScore) { bestScore = s; bestA = a; }
            }
            return bestA;
        }

        // ── Expand: fill in legal mask + prior on a fresh node ──────────────
        function expand(node, priorFn) {
            const legal = env.legalActions();
            for (let i = 0; i < legal.length; i++) node.legal[legal[i]] = 1;

            if (priorFn) {
                const obs = env.observe();
                const probs = priorFn(obs, legal);
                let s = 0;
                for (let a = 0; a < numActions; a++) {
                    if (node.legal[a]) {
                        const p = Math.max(0, probs[a] || 0);
                        node.P[a] = p; s += p;
                    }
                }
                if (s > 0) {
                    for (let a = 0; a < numActions; a++) node.P[a] /= s;
                } else {
                    // Net produced all zeros over legal — fall back to uniform.
                    const u = 1 / Math.max(1, legal.length);
                    for (let i = 0; i < legal.length; i++) node.P[legal[i]] = u;
                }
            } else {
                const u = 1 / Math.max(1, legal.length);
                for (let i = 0; i < legal.length; i++) node.P[legal[i]] = u;
            }
            node.expanded = true;
        }

        // ── Random rollout: used when no valueFn provided ───────────────────
        function randomRollout(depth, gamma) {
            let g = 0, discount = 1;
            for (let i = 0; i < depth; i++) {
                const legal = env.legalActions();
                if (legal.length === 0) break;
                const a = legal[(rand() * legal.length) | 0];
                const out = env.step(a);
                g += discount * out.reward;
                discount *= gamma;
                if (out.done) {
                    // No bootstrap; the terminal reward is fully captured.
                    return g;
                }
            }
            return g;
        }

        let root = newNode();
        let rootSnapshot = null;
        let lastSearchObs = null;

        function search(searchOpts) {
            searchOpts = searchOpts || {};
            const iterations = searchOpts.iterations || 100;
            const priorFn  = searchOpts.priorFn || null;
            const valueFn  = searchOpts.valueFn || null;
            const gamma    = searchOpts.gamma != null ? searchOpts.gamma : 0.99;
            const rolloutDepth = searchOpts.rolloutDepth || 8;
            // Dirichlet exploration noise on the root prior (AlphaZero). Off
            // by default — pass dirichletAlpha (>0) and dirichletEpsilon
            // (∈[0,1]) to enable.
            const dAlpha   = searchOpts.dirichletAlpha   || 0;
            const dEps     = searchOpts.dirichletEpsilon || 0;

            // Capture root state once per search.
            rootSnapshot = env.snapshot();
            lastSearchObs = env.observe();

            // Re-expand the root each search if not already.
            if (!root.expanded) expand(root, priorFn);

            // Mix Dirichlet noise into the root prior so MCTS occasionally
            // visits actions the net thinks are bad — critical for breaking
            // out of cold-start traps in sparse-reward environments. Done
            // *after* expansion so we operate on the normalized prior.
            if (dAlpha > 0 && dEps > 0) {
                const legalIdx = [];
                for (let a = 0; a < numActions; a++) if (root.legal[a]) legalIdx.push(a);
                if (legalIdx.length > 0) {
                    const noise = sampleDirichlet(legalIdx, dAlpha);
                    for (let i = 0; i < legalIdx.length; i++) {
                        const a = legalIdx[i];
                        root.P[a] = (1 - dEps) * root.P[a] + dEps * noise[a];
                    }
                }
            }

            for (let it = 0; it < iterations; it++) {
                env.restore(rootSnapshot);

                // Selection: walk down through expanded children.
                const path = [];
                let node = root;
                while (true) {
                    const a = pickAction(node);
                    if (a < 0) break;             // no legal action (shouldn't happen)
                    path.push({ node, action: a });
                    let child = node.children[a];
                    if (child === null) {
                        // Edge not yet expanded — take the action and create the child.
                        const out = env.step(a);
                        child = newNode();
                        child.rewardFromParent = out.reward;
                        child.terminal = !!out.done;
                        node.children[a] = child;
                        node = child;
                        break;
                    } else {
                        // Replay the action on the live env.
                        env.step(a);
                        node = child;
                        if (node.terminal) break;
                    }
                }

                // Evaluation.
                let leafValue;
                if (node.terminal) {
                    leafValue = 0;
                } else {
                    if (!node.expanded) expand(node, priorFn);
                    if (valueFn) {
                        leafValue = valueFn(env.observe());
                    } else {
                        leafValue = randomRollout(rolloutDepth, gamma);
                    }
                }

                // Backup: G_t = r_{t+1} + γ * G_{t+1}, but here each edge has its
                // own r and we want each Q(s,a) estimated as the discounted return
                // starting from taking a in s. Walk path backward, accumulating G.
                let g = leafValue;
                for (let i = path.length - 1; i >= 0; i--) {
                    const { node: parent, action: a } = path[i];
                    const child = parent.children[a];
                    g = child.rewardFromParent + gamma * g;
                    parent.N[a] += 1;
                    parent.W[a] += g;
                    parent.visits += 1;
                }
            }

            // Restore live env to where the user left it before search.
            env.restore(rootSnapshot);

            // Choose action by visit count (greedy).
            let bestA = -1, bestN = -1;
            for (let a = 0; a < numActions; a++) {
                if (root.N[a] > bestN) { bestN = root.N[a]; bestA = a; }
            }
            return bestA;
        }

        function rootVisits() {
            const v = new Float32Array(numActions);
            let s = 0;
            for (let a = 0; a < numActions; a++) { v[a] = root.N[a]; s += v[a]; }
            if (s > 0) for (let a = 0; a < numActions; a++) v[a] /= s;
            return v;
        }

        function rootObs() { return lastSearchObs; }

        function advanceRoot(action) {
            const child = root.children[action];
            root = child || newNode();
        }

        function reset() { root = newNode(); rootSnapshot = null; }

        return { search, rootVisits, rootObs, advanceRoot, reset, get numActions() { return numActions; } };
    }

    global.MctsJs = { create };
})(typeof window !== 'undefined' ? window : globalThis);
