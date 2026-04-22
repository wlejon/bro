// storage.js — High score persistence (wrapped around lib/storage).
var T = T || {};

T.Storage = (function() {
    var s = Storage.create("touchdown");
    s.load({ highScore: 0 });
    return {
        get highScore() { return s.get("highScore") || 0; },
        set highScore(v) { s.set("highScore", v); },
        load: function() { /* already loaded */ },
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
