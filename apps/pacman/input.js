// input.js — simple keyboard buffering for pacman movement
var P = P || {};

P.Input = {
    pendingDir: -1, // queued direction from latest press
    keysDown: {},

    onKeyDown: function(key) {
        if (key === "ArrowRight") this.pendingDir = 0;
        else if (key === "ArrowLeft") this.pendingDir = 1;
        else if (key === "ArrowUp") this.pendingDir = 2;
        else if (key === "ArrowDown") this.pendingDir = 3;
        this.keysDown[key] = true;
    },

    onKeyUp: function(key) {
        this.keysDown[key] = false;
    },

    consume: function() {
        var d = this.pendingDir;
        this.pendingDir = -1;
        return d;
    },

    reset: function() {
        this.pendingDir = -1;
        this.keysDown = {};
    }
};
