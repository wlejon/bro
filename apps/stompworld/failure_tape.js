// failure_tape.js — short-term anti-repetition memory for the display
// agent. The trainer worker and data workers explore freely (Dirichlet
// noise, no failure memory). The live worker is different: it should
// always be playing the *best* version of what it knows, and we should
// never watch it die the same way twice in a row.
//
// Mechanism: every decision, the worker computes a coarse signature of
// the current state (player tile column, tile row, onGround flag, vx
// sign) and records it alongside the action it took. On a failed
// terminal (death/stall/timeout), the last `lookback` (sig, action)
// pairs are inserted into the tape. On the next `priorFn` call MCTS
// asks the tape for per-action multipliers at the current sig — actions
// that are on the tape get their prior scaled down before MCTS expands
// them, which makes the search visit alternatives.
//
// The tape is a FIFO ring of (sig, action) entries, capped at maxEntries.
// When an entry falls out, its corresponding count in the sig→action
// map is decremented and the sig pruned if empty. So the tape "forgets"
// as fresh failures arrive — the agent gets a clean slate after a while
// of not failing in the same way.

(function (global) {
    'use strict';

    function create(opts) {
        opts = opts || {};
        const maxEntries = opts.maxEntries != null ? opts.maxEntries : 200;
        const lookback   = opts.lookback   != null ? opts.lookback   : 8;
        const penalty    = opts.penalty    != null ? opts.penalty    : 0.1;
        const numActions = opts.numActions != null ? opts.numActions : 6;
        const tile       = opts.tile       != null ? opts.tile       : 32;

        // Map<sig (string), Map<action (int), count (int)>>
        const map = new Map();
        // Insertion-order ring of {sig, action} for FIFO eviction.
        const order = [];

        function buildSig(sim) {
            const p = sim.player;
            const col = Math.floor(p.x / tile);
            const row = Math.floor(p.y / tile);
            const og  = p.onGround ? 1 : 0;
            const vxSign = p.vx > 8 ? 1 : (p.vx < -8 ? -1 : 0);
            return col + ',' + row + ',' + og + ',' + vxSign;
        }

        function evict() {
            while (order.length > maxEntries) {
                const e = order.shift();
                const m = map.get(e.sig);
                if (!m) continue;
                const c = (m.get(e.action) || 0) - 1;
                if (c <= 0) m.delete(e.action);
                else m.set(e.action, c);
                if (m.size === 0) map.delete(e.sig);
            }
        }

        function addEntry(sig, action) {
            let inner = map.get(sig);
            if (!inner) { inner = new Map(); map.set(sig, inner); }
            inner.set(action, (inner.get(action) || 0) + 1);
            order.push({ sig, action });
            evict();
        }

        // Walk back up to `lookback` (sig, action) pairs from a parallel
        // pair of arrays (sigList[i] = sig at decision i, actions[i] =
        // action taken at that decision) and add them all to the tape.
        function recordFailure(sigList, actions) {
            const len = Math.min(sigList.length, actions.length);
            const start = Math.max(0, len - lookback);
            for (let i = start; i < len; i++) {
                addEntry(sigList[i], actions[i]);
            }
        }

        function biasFor(sig, legal) {
            const inner = map.get(sig);
            if (!inner) return null;
            const out = new Float32Array(numActions);
            for (let i = 0; i < numActions; i++) out[i] = 1;
            // Multiplicative penalty, stronger with repeat count but
            // floors at penalty^3 so we never absolutely forbid an action.
            for (let i = 0; i < legal.length; i++) {
                const a = legal[i];
                const c = inner.get(a);
                if (c) {
                    const k = Math.min(3, c);
                    out[a] = Math.pow(penalty, k);
                }
            }
            return out;
        }

        function clear() { map.clear(); order.length = 0; }
        function entries() { return order.length; }
        function sigs() { return map.size; }

        return {
            buildSig, addEntry, recordFailure, biasFor,
            clear, entries, sigs,
        };
    }

    global.FailureTape = { create };
})(typeof window !== 'undefined' ? window : globalThis);
