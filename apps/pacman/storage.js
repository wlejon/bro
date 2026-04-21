// storage.js — high score persistence
var P = P || {};

P.Storage = {
    highScore: 0,

    load: function() {
        try {
            var s = localStorage.getItem("pacman_highscore");
            if (s) this.highScore = parseInt(s, 10) || 0;
        } catch(e) {}
    },

    save: function() {
        try {
            localStorage.setItem("pacman_highscore", String(this.highScore));
        } catch(e) {}
    },

    maybeSetHigh: function(score) {
        if (score > this.highScore) {
            this.highScore = score;
            this.save();
            return true;
        }
        return false;
    }
};
