// =============================================================================
// Tape measure — two-click distance readout. No geometry is produced; the
// tool is HUD-only, scoped to the session (not persisted in the scene or
// project file).
//
// Pure-state module. The app walks through begin → update* → commit/cancel;
// input routing and HUD formatting live in app.js.
// =============================================================================

(function (global) {
    'use strict';

    function createState() {
        return {
            active: false,
            from:   null,    // [x, y, z] — first click
            to:     null,    // [x, y, z] — live cursor or second click
        };
    }

    function begin(state, pos) {
        state.active = true;
        state.from   = pos.slice();
        state.to     = pos.slice();
    }

    function update(state, pos) {
        if (!state.active) return;
        state.to = pos.slice();
    }

    function distance(state) {
        if (!state.active || !state.from || !state.to) return 0;
        const dx = state.to[0] - state.from[0];
        const dy = state.to[1] - state.from[1];
        const dz = state.to[2] - state.from[2];
        return Math.sqrt(dx * dx + dy * dy + dz * dz);
    }

    function commit(state) {
        const d = distance(state);
        clear(state);
        return d;
    }

    function cancel(state) {
        clear(state);
    }

    function clear(state) {
        state.active = false;
        state.from   = null;
        state.to     = null;
    }

    global.TapeTool = {
        createState, begin, update, distance, commit, cancel,
    };

})(typeof globalThis !== 'undefined' ? globalThis : this);
