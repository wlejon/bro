// storage.js — High score persistence (wraps lib/storage).
var A = A || {};

A.Storage = (function() {
    var s = Storage.create("asteroids");
    return {
        get highScore() { return s.get("highScore") || 0; },
        set highScore(v) { s.set("highScore", v); },
        load: function() { s.load({ highScore: 0 }); },
        save: function() { s.save(); },
        maybeUpdate: function(score) {
            if (score > (s.get("highScore") || 0)) {
                s.set("highScore", score);
                s.save();
                return true;
            }
            return false;
        }
    };
})();
