// storage.js — High score persistence
var T = T || {};

T.Storage = {
    highScore: 0,

    load: function() {
        try {
            var s = localStorage.getItem("touchdown_highscore");
            if (s) {
                var n = parseInt(s, 10);
                if (!isNaN(n)) this.highScore = n;
            }
        } catch(e) {}
    },

    save: function() {
        try {
            localStorage.setItem("touchdown_highscore", String(this.highScore));
        } catch(e) {}
    },

    maybeUpdate: function(score) {
        if (score > this.highScore) {
            this.highScore = score;
            this.save();
            return true;
        }
        return false;
    }
};
