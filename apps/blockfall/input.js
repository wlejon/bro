// input.js — Action binding (bro.settings integration) and DAS handling
var T = T || {};

T.Input = {
    hasBro: false,

    ACTIONS: [
        { name: "move_left",  label: "Move Left",  defaults: ["a", "ArrowLeft"] },
        { name: "move_right", label: "Move Right", defaults: ["d", "ArrowRight"] },
        { name: "soft_drop",  label: "Soft Drop",  defaults: ["s", "ArrowDown"] },
        { name: "hard_drop",  label: "Hard Drop",  defaults: [" "] },
        { name: "rotate_cw",  label: "Rotate CW",  defaults: ["w", "ArrowUp"] },
        { name: "rotate_ccw", label: "Rotate CCW", defaults: ["q", "z"] },
        { name: "hold_piece", label: "Hold",        defaults: ["c", "Shift"] },
        { name: "pause_game", label: "Pause",       defaults: ["Escape", "p"] }
    ],

    controls: {},

    init: function() {
        this.hasBro = (typeof bro !== "undefined" && bro.settings &&
                       typeof bro.settings.defineAction === "function");
        if (this.hasBro) {
            for (var i = 0; i < this.ACTIONS.length; i++) {
                var a = this.ACTIONS[i];
                bro.settings.defineAction(a.name, a.defaults);
            }
        } else {
            try {
                var c = localStorage.getItem("tetris_controls");
                if (c) this.controls = JSON.parse(c);
            } catch(e) {}
            for (var i = 0; i < this.ACTIONS.length; i++) {
                var a = this.ACTIONS[i];
                if (!this.controls[a.name]) this.controls[a.name] = a.defaults[0];
            }
        }
    },

    getKeys: function(actionName) {
        if (this.hasBro) {
            try { return bro.settings.getActionKeys(actionName) || []; } catch(e) {}
        }
        return [this.controls[actionName] || ""];
    },

    rebind: function(actionName, keys) {
        if (this.hasBro) {
            try { bro.settings.rebindAction(actionName, keys); } catch(e) {}
        } else {
            this.controls[actionName] = keys[0];
            try { localStorage.setItem("tetris_controls", JSON.stringify(this.controls)); } catch(e) {}
        }
    },

    resetAll: function() {
        for (var i = 0; i < this.ACTIONS.length; i++) {
            this.rebind(this.ACTIONS[i].name, this.ACTIONS[i].defaults);
        }
    },

    getActionForKey: function(key) {
        if (this.hasBro) {
            try { return bro.settings.getKeyAction(key) || null; } catch(e) {}
            return null;
        }
        for (var name in this.controls) {
            if (this.controls[name] === key) return name;
        }
        return null;
    },

    keyDisplayName: function(key) {
        if (key === " ") return "Space";
        if (key === "ArrowLeft") return "\u2190";
        if (key === "ArrowRight") return "\u2192";
        if (key === "ArrowUp") return "\u2191";
        if (key === "ArrowDown") return "\u2193";
        if (key === "Escape") return "Esc";
        if (key.length === 1) return key.toUpperCase();
        return key;
    },

    // DAS (Delayed Auto Shift) state
    das: { dir: 0, timer: 0, active: false, key: "", delay: 167, arr: 33 },
    softDrop: { active: false, timer: 0, rate: 30 },
    keysDown: {},

    resetDAS: function() {
        this.das.dir = 0;
        this.das.timer = 0;
        this.das.active = false;
        this.das.key = "";
        this.softDrop.active = false;
        this.softDrop.timer = 0;
        this.keysDown = {};
    }
};
