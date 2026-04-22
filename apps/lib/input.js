// input.js — keyboard state + named actions with bro.settings rebinding.
//
// Usage:
//   <script src="../lib/input.js"></script>
//   Input.init([
//       { name: "fire",   label: "Fire",   defaults: [" "] },
//       { name: "left",   label: "Left",   defaults: ["ArrowLeft", "a"] },
//       { name: "pause",  label: "Pause",  defaults: ["Escape"] },
//   ]);
//   Input.attach(window);                    // installs key listeners
//   if (Input.down("left")) {...}            // held?
//   if (Input.pressed("fire")) {...}         // rising edge (consume once)
//   Input.onAction((action, phase, key) => {});
//
// When `bro.settings.defineAction` is available, bindings persist via the
// engine's action system (rebindable in Controls UI). Otherwise falls back
// to localStorage so rebinds still survive reloads in a plain browser.

(function (global) {
    'use strict';

    const state = {
        actions:   [],        // [{name, label, defaults}]
        hasBro:    false,
        controls:  {},        // action → key (fallback mode)
        listeners: [],        // (action, phase, key) handlers
        held:      {},        // action → true while pressed
        edge:      {},        // action → true once until consumed via pressed()
        raw:       {},        // key → true (for debugging / raw access)
        attached:  false,
        storageKey: 'input_controls',
    };

    function init(actions, opts) {
        opts = opts || {};
        state.actions    = actions || [];
        state.storageKey = opts.storageKey || state.storageKey;
        state.hasBro = (typeof bro !== 'undefined' && bro.settings &&
                        typeof bro.settings.defineAction === 'function');
        if (state.hasBro) {
            for (const a of state.actions) {
                bro.settings.defineAction(a.name, a.defaults.slice());
            }
        } else {
            try {
                const c = localStorage.getItem(state.storageKey);
                if (c) state.controls = JSON.parse(c);
            } catch (e) {}
            for (const a of state.actions) {
                if (!state.controls[a.name]) {
                    state.controls[a.name] = a.defaults[0];
                }
            }
        }
    }

    function getKeys(actionName) {
        if (state.hasBro) {
            try { return bro.settings.getActionKeys(actionName) || []; }
            catch (e) {}
        }
        const k = state.controls[actionName];
        return k ? [k] : [];
    }

    function rebind(actionName, keys) {
        if (state.hasBro) {
            try { bro.settings.rebindAction(actionName, keys); } catch (e) {}
        } else {
            state.controls[actionName] = keys[0];
            try {
                localStorage.setItem(state.storageKey, JSON.stringify(state.controls));
            } catch (e) {}
        }
    }

    function actionForKey(key) {
        if (state.hasBro) {
            try { return bro.settings.getKeyAction(key) || null; } catch (e) {}
            return null;
        }
        for (const a of state.actions) {
            if (state.controls[a.name] === key) return a.name;
        }
        return null;
    }

    function keyDisplay(key) {
        if (!key) return '';
        if (key === ' ') return 'Space';
        if (key === 'ArrowLeft')  return '\u2190';
        if (key === 'ArrowRight') return '\u2192';
        if (key === 'ArrowUp')    return '\u2191';
        if (key === 'ArrowDown')  return '\u2193';
        if (key === 'Escape') return 'Esc';
        if (key.length === 1) return key.toUpperCase();
        return key;
    }

    function onKeyDown(e) {
        const key = e.key;
        if (state.raw[key]) return; // ignore OS auto-repeat
        state.raw[key] = true;
        const a = actionForKey(key);
        if (a) {
            state.held[a] = true;
            state.edge[a] = true;
        }
        for (const cb of state.listeners) cb(a, 'down', key);
    }

    function onKeyUp(e) {
        const key = e.key;
        state.raw[key] = false;
        const a = actionForKey(key);
        if (a) state.held[a] = false;
        for (const cb of state.listeners) cb(a, 'up', key);
    }

    function attach(target) {
        if (state.attached) return;
        state.attached = true;
        target = target || window;
        target.addEventListener('keydown', onKeyDown);
        target.addEventListener('keyup',   onKeyUp);
    }

    function clear() {
        state.held = {};
        state.edge = {};
        state.raw  = {};
    }

    global.Input = {
        init, attach, clear,
        getKeys, rebind, actionForKey, keyDisplay,
        down:    (name) => !!state.held[name],
        pressed: (name) => {
            if (!state.edge[name]) return false;
            state.edge[name] = false;
            return true;
        },
        rawDown: (key) => !!state.raw[key],
        onAction: (cb) => { state.listeners.push(cb); },
        actions: () => state.actions.slice(),
    };
})(typeof window !== 'undefined' ? window : globalThis);
