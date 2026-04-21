// storage.js — Settings persistence and high score tracking
var T = T || {};

T.Storage = {
    settings: {
        startLevel: 1,
        sfxVol: 80,
        musicVol: 70,
        ghostPiece: true,
        gridLines: true
    },

    load: function() {
        try {
            var s = localStorage.getItem("tetris_settings");
            if (s) {
                var parsed = JSON.parse(s);
                var settings = this.settings;
                for (var k in parsed) {
                    if (settings.hasOwnProperty(k)) settings[k] = parsed[k];
                }
            }
        } catch(e) {}
    },

    save: function() {
        try {
            localStorage.setItem("tetris_settings", JSON.stringify(this.settings));
        } catch(e) {}
    },

    MAX_SCORES: 10,

    loadHighScores: function() {
        try {
            var s = localStorage.getItem("tetris_highscores");
            if (s) return JSON.parse(s);
        } catch(e) {}
        return { marathon: [], sprint: [], ultra: [] };
    },

    saveHighScores: function(scores) {
        try {
            localStorage.setItem("tetris_highscores", JSON.stringify(scores));
        } catch(e) {}
    },

    addHighScore: function(mode, entry) {
        var scores = this.loadHighScores();
        if (!scores[mode]) scores[mode] = [];
        scores[mode].push(entry);
        if (mode === "sprint") {
            scores[mode].sort(function(a, b) { return a.time - b.time; });
        } else {
            scores[mode].sort(function(a, b) { return b.score - a.score; });
        }
        scores[mode] = scores[mode].slice(0, this.MAX_SCORES);
        this.saveHighScores(scores);
        return scores;
    },

    isHighScore: function(mode, value) {
        var scores = this.loadHighScores();
        if (!scores[mode] || scores[mode].length < this.MAX_SCORES) return true;
        if (mode === "sprint") {
            return value < scores[mode][scores[mode].length - 1].time;
        }
        return value > scores[mode][scores[mode].length - 1].score;
    }
};
