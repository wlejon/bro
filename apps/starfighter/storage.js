// storage.js — High score persistence (wraps lib/storage).
var N = N || {};

N.Storage = (function() {
    var s = Storage.create("starfighter");
    return {
        get highScore() { return s.get("highScore") || 0; },
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
