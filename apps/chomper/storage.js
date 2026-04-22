// storage.js — high score persistence (wraps lib/storage).
var P = P || {};

P.Storage = (function() {
    var s = Storage.create("chomper");
    return {
        get highScore() { return s.get("highScore") || 0; },
        set highScore(v) { s.set("highScore", v); },
        load: function() { s.load({ highScore: 0 }); },
        save: function() { s.save(); },
        maybeSetHigh: function(score) {
            if (score > (s.get("highScore") || 0)) {
                s.set("highScore", score);
                s.save();
                return true;
            }
            return false;
        }
    };
})();
