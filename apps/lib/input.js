// input.js — keyboard/mouse state + named actions with bro.settings rebinding.
//
// Usage:
//   <script src="../lib/input.js"></script>
//   Input.init(Input.STANDARD_ACTIONS);         // or custom list
//   Input.attach(window);                       // key + mouse listeners
//   if (Input.down("left")) {...}               // held?
//   if (Input.pressed("primary")) {...}         // rising edge (consume once)
//   Input.onAction((action, phase, key) => {});
//
// Keys are stored/returned as "web-key" strings. Mouse buttons use the
// virtual strings "Mouse0" (left), "Mouse1" (middle), "Mouse2" (right);
// wheel uses "WheelUp"/"WheelDown". These are round-trippable through the
// engine's bro.settings action system and are bindable in System →
// Input. The standard action vocabulary (below) is WASD-primary with
// arrows and mouse as secondary so any game that uses it gets consistent
// controls out of the box.

(function (global) {
    'use strict';

    // Shared vocabulary. Games should use these names when possible so
    // the System → Input panel groups bindings meaningfully across apps.
    // WASD is the primary set; arrow keys are secondary. Mouse0 is
    // primary action; Space is a secondary primary so menus still work.
    const STANDARD_ACTIONS = [
        { name: 'up',        label: 'Up',        defaults: ['w', 'ArrowUp']    },
        { name: 'down',      label: 'Down',      defaults: ['s', 'ArrowDown']  },
        { name: 'left',      label: 'Left',      defaults: ['a', 'ArrowLeft']  },
        { name: 'right',     label: 'Right',     defaults: ['d', 'ArrowRight'] },
        { name: 'primary',   label: 'Fire',      defaults: [' ', 'Mouse0']     },
        { name: 'secondary', label: 'Alt Fire',  defaults: ['Shift', 'Mouse2'] },
        { name: 'pause',     label: 'Pause',     defaults: ['Escape', 'p']     },
        { name: 'confirm',   label: 'Confirm',   defaults: ['Enter']           },
    ];

    const state = {
        actions:   [],        // [{name, label, defaults}]
        hasBro:    false,
        controls:  {},        // action → key[] (fallback mode)
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
                    state.controls[a.name] = a.defaults.slice();
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
        return k ? k.slice() : [];
    }

    function rebind(actionName, keys) {
        if (state.hasBro) {
            try { bro.settings.rebindAction(actionName, keys); } catch (e) {}
        } else {
            state.controls[actionName] = keys.slice();
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
        for (const name in state.controls) {
            if ((state.controls[name] || []).indexOf(key) >= 0) return name;
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
        if (key === 'Escape')     return 'Esc';
        if (key === 'Mouse0')     return 'L-Click';
        if (key === 'Mouse1')     return 'M-Click';
        if (key === 'Mouse2')     return 'R-Click';
        if (key === 'WheelUp')    return 'Wheel\u2191';
        if (key === 'WheelDown')  return 'Wheel\u2193';
        if (key.length === 1) return key.toUpperCase();
        return key;
    }

    function fireAction(key, phase) {
        const a = actionForKey(key);
        if (a) {
            if (phase === 'down') {
                state.held[a] = true;
                state.edge[a] = true;
            } else if (phase === 'up') {
                state.held[a] = false;
            }
        }
        for (const cb of state.listeners) cb(a, phase, key);
    }

    function onKeyDown(e) {
        const key = e.key;
        if (state.raw[key]) return; // ignore OS auto-repeat
        state.raw[key] = true;
        fireAction(key, 'down');
    }

    function onKeyUp(e) {
        const key = e.key;
        state.raw[key] = false;
        fireAction(key, 'up');
    }

    function onMouseDown(e) {
        const key = 'Mouse' + e.button;
        state.raw[key] = true;
        fireAction(key, 'down');
    }

    function onMouseUp(e) {
        const key = 'Mouse' + e.button;
        state.raw[key] = false;
        fireAction(key, 'up');
    }

    // Wheel events are momentary — no held state, just edge pulses.
    function onWheel(e) {
        const key = e.deltaY < 0 ? 'WheelUp' : 'WheelDown';
        fireAction(key, 'down');
        fireAction(key, 'up');
    }

    function attach(target) {
        if (state.attached) return;
        state.attached = true;
        target = target || window;
        target.addEventListener('keydown',   onKeyDown);
        target.addEventListener('keyup',     onKeyUp);
        target.addEventListener('mousedown', onMouseDown);
        target.addEventListener('mouseup',   onMouseUp);
        target.addEventListener('wheel',     onWheel, { passive: true });
    }

    function clear() {
        state.held = {};
        state.edge = {};
        state.raw  = {};
    }

    global.Input = {
        init, attach, clear,
        getKeys, rebind, actionForKey, keyDisplay,
        STANDARD_ACTIONS,
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
