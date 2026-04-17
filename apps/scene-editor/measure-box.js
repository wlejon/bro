// =============================================================================
// Measurement Box (VCB — Value Control Box).
//
// SketchUp-style precision input: type a number during a tool drag, press
// Enter, and the current operation commits at that exact distance. Post-
// commit, the same VCB can re-apply the last operation at a new distance,
// matching the SketchUp "pull then type 3 Enter" workflow.
//
// This module is pure state — parsing + validation. Integration with a tool
// (feeding keys in from keydown handlers, reading the current value during
// drag, stashing last-op metadata for re-application) lives in the app.
//
// Accepted characters: digits 0-9, '.', '-' (leading only), Backspace,
// Enter, Escape. Anything else returns action='ignored' and doesn't mutate.
// =============================================================================

(function (global) {
    'use strict';

    // Is `s` a well-formed distance string? Rejects empty, orphan '-',
    // orphan '.', and multi-dot. A leading minus is only valid if followed
    // by digits.
    function isValidNumber(s) {
        if (!s) return false;
        if (s === '-' || s === '.') return false;
        if (s === '-.') return false;
        // JS parseFloat is permissive; an explicit regex guards against it
        // accepting trailing garbage that the caller typed by accident.
        return /^-?\d*\.?\d+$/.test(s);
    }

    function parseValue(s) {
        if (!isValidNumber(s)) return null;
        return parseFloat(s);
    }

    // Parse "A,B" (e.g. rectangle WxH input). Returns [a, b] or null. A
    // single-number buffer returns null — callers that want to accept either
    // form should fall back to parseValue themselves.
    function parseValuePair(s) {
        if (!s) return null;
        const comma = s.indexOf(',');
        if (comma < 0) return null;
        const a = s.substring(0, comma);
        const b = s.substring(comma + 1);
        if (!isValidNumber(a) || !isValidNumber(b)) return null;
        return [parseFloat(a), parseFloat(b)];
    }

    function createState() {
        return {
            buffer: '',
            active: false,       // is there a drag or post-commit window?
            lastOp: null,        // { apply: fn, meta: any }
            // Incremented whenever the buffer or activity state changes —
            // UI layers can diff against their last-rendered tick to avoid
            // unnecessary DOM work.
            tick: 0,
        };
    }

    // Feed a keyboard event's `key` string. Returns one of:
    //   'append'   — buffer changed, caller may want to re-preview
    //   'commit'   — Enter pressed with a valid buffer; caller should apply
    //   'cancel'   — Escape pressed; caller should discard
    //   'ignored'  — key not recognized, no state change
    //
    // Side effect: mutates `state.buffer` and bumps `state.tick` on change.
    // When the tool consuming the VCB expects a pair (e.g. rectangle WxH),
    // set `state.pairMode = true` so feedKey accepts a comma and validates
    // the buffer as two comma-separated numbers on commit. Single-number
    // tools (push/pull) leave pairMode unset and behavior is unchanged.
    function feedKey(state, key) {
        if (!state.active) return 'ignored';
        if (key === 'Enter') {
            // Only commit with a valid buffer. Empty + Enter is a no-op so
            // the user can press Enter to dismiss without acting.
            const valid = state.pairMode
                ? parseValuePair(state.buffer) !== null
                : isValidNumber(state.buffer);
            if (valid) return 'commit';
            return 'ignored';
        }
        if (key === 'Escape') {
            state.buffer = '';
            state.tick++;
            return 'cancel';
        }
        if (key === 'Backspace') {
            if (state.buffer.length === 0) return 'ignored';
            state.buffer = state.buffer.slice(0, -1);
            state.tick++;
            return 'append';
        }
        // Digit
        if (key.length === 1 && key >= '0' && key <= '9') {
            state.buffer += key;
            state.tick++;
            return 'append';
        }
        // Minus sign — only accepted as the first character.
        if (key === '-') {
            if (state.buffer.length !== 0) return 'ignored';
            state.buffer = '-';
            state.tick++;
            return 'append';
        }
        // Decimal point — at most one per number (in pair mode, one before
        // and one after the comma are both allowed).
        if (key === '.') {
            let last = state.buffer;
            if (state.pairMode) {
                const i = state.buffer.lastIndexOf(',');
                if (i >= 0) last = state.buffer.substring(i + 1);
            }
            if (last.indexOf('.') !== -1) return 'ignored';
            if (last === '-') return 'ignored';
            state.buffer += '.';
            state.tick++;
            return 'append';
        }
        // Comma — only in pair mode, and only after a valid first number.
        if (key === ',' && state.pairMode) {
            if (state.buffer.indexOf(',') !== -1) return 'ignored';
            if (!isValidNumber(state.buffer)) return 'ignored';
            state.buffer += ',';
            state.tick++;
            return 'append';
        }
        return 'ignored';
    }

    function setPairMode(state, pairMode) {
        if (state.pairMode === pairMode) return;
        state.pairMode = pairMode;
        state.tick++;
    }

    function clear(state) {
        if (state.buffer.length === 0) return;
        state.buffer = '';
        state.tick++;
    }

    function setActive(state, active) {
        if (state.active === active) return;
        state.active = active;
        state.tick++;
    }

    function setLastOp(state, lastOp) {
        state.lastOp = lastOp;
        state.tick++;
    }

    function clearLastOp(state) {
        if (!state.lastOp) return;
        state.lastOp = null;
        state.tick++;
    }

    global.MeasureBox = {
        createState,
        feedKey,
        clear,
        setActive,
        setLastOp,
        clearLastOp,
        setPairMode,
        parseValue,
        parseValuePair,
        isValidNumber,
    };

})(typeof globalThis !== 'undefined' ? globalThis : this);
