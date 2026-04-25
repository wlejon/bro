// storage.js — settings + per-mode high scores, backed by lib/storage.js.
//
// Three high-score tables (marathon/sprint/ultra). Sprint is timed
// (lower is better), so it uses ascending sort on `time`; the others
// sort descending on `score`.
var T = T || {};

T.Storage = (function () {
    'use strict';

    var DEFAULTS = {
        startLevel: 1,
        sfxVol:     80,
        musicVol:   70,
        ghostPiece: true,
        gridLines:  true,
    };

    var settingsStore = Storage.create('blockfall');

    var hs = {
        marathon: Storage.highscores('blockfall:marathon', 10),
        sprint:   Storage.highscores('blockfall:sprint',   10, { field: 'time', ascending: true }),
        ultra:    Storage.highscores('blockfall:ultra',    10),
    };

    return {
        // Direct mutable settings object (callers read+write fields).
        settings: Object.assign({}, DEFAULTS),

        load: function () {
            var data = settingsStore.load(DEFAULTS);
            for (var k in data) this.settings[k] = data[k];
        },
        save: function () {
            for (var k in this.settings) settingsStore.set(k, this.settings[k]);
            settingsStore.save();
        },

        loadHighScores: function () {
            return {
                marathon: hs.marathon.list(),
                sprint:   hs.sprint.list(),
                ultra:    hs.ultra.list(),
            };
        },
        addHighScore: function (mode, entry) {
            if (hs[mode]) hs[mode].add(entry);
            return this.loadHighScores();
        },
        isHighScore: function (mode, value) {
            return hs[mode] ? hs[mode].qualifies(value) : true;
        },
    };
})();
