// best_crop.js — main-thread ranked pool of recent trajectories.
//
// Both the live worker and the MCTS data workers ship a `trajectory`
// message after every episode they finish: a sim snapshot of the start
// state, the action sequence, the total return, the search depth, and
// the terminal reason. We accept all of them, score them, and keep the
// top-K. The pool then feeds the live worker periodically as a *seed*
// for the next display episode — the live worker can either restore the
// snapshot only (Mode A) or restore + replay an action prefix (Mode B,
// which lands the live agent in a known-rich state mid-trajectory and
// has it search from there). That's the "search on top of search" the
// architecture is built around.
//
// Score weights:
//   totalReturn     — primary; higher is better (return is in [-1, ~5])
//   depthBonus * d  — favors deeper-search trajectories slightly so the
//                     pool prefers high-confidence data when returns tie
//   ageDecay * t    — gradually demote old entries so weights drift
//                     doesn't leave us seeding from a bygone net version

(function (global) {
    'use strict';

    function create(opts) {
        opts = opts || {};
        const capacity   = opts.capacity   != null ? opts.capacity   : 32;
        const depthBonus = opts.depthBonus != null ? opts.depthBonus : 0.001;
        const ageDecay   = opts.ageDecay   != null ? opts.ageDecay   : 0.0001;
        // Topk window for sampling: pick uniformly from the top-K by score.
        const sampleTopK = opts.sampleTopK != null ? opts.sampleTopK : 8;

        // items[i] = {
        //   startSnap, actions[], decisions, totalReturn, searchDepth,
        //   reason, source, bestX, bornTick
        // }
        const items = [];
        let tick = 0;
        let acceptedTotal = 0;

        function score(item) {
            return item.totalReturn
                 + depthBonus * (item.searchDepth || 0)
                 - ageDecay   * (tick - item.bornTick);
        }

        function rerank() {
            items.sort((a, b) => score(b) - score(a));
            while (items.length > capacity) items.pop();
        }

        function ingest(item) {
            // Reject obviously unusable entries (no startSnap, no actions).
            if (!item || !item.startSnap || !item.actions) return;
            item.bornTick = tick;
            items.push(item);
            acceptedTotal++;
            rerank();
        }

        function step() { tick++; }

        function pick(rng) {
            if (items.length === 0) return null;
            rng = rng || Math.random;
            const k = Math.min(sampleTopK, items.length);
            const idx = Math.floor(rng() * k);
            return items[idx];
        }

        function topReturn() { return items.length ? items[0].totalReturn : 0; }
        function size() { return items.length; }
        function totalAccepted() { return acceptedTotal; }
        function clear() { items.length = 0; }

        return {
            ingest, pick, step, size, topReturn, totalAccepted, clear,
            get items() { return items; },
        };
    }

    global.BestCrop = { create };
})(typeof window !== 'undefined' ? window : globalThis);
