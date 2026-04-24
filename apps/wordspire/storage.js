// storage.js — wordspire settings + per-mode high scores + top words.
'use strict';
var W = window.W = window.W || {};

W.Storage = (function () {
    var store = Storage.create('wordspire');
    var defaults = {
        sfxVol: 80,
        musicVol: 60,
        difficulty: 1   // 0 easy, 1 normal, 2 hard (burning tile frequency)
    };
    var settings = store.load(defaults);

    var hsClassic = Storage.highscores('wordspire:classic', 10);
    var hsTimed   = Storage.highscores('wordspire:timed',   10);
    var hsPuzzle  = Storage.highscores('wordspire:puzzle',  10);

    // Top words table ranked by score-per-word.
    var topWords  = Storage.highscores('wordspire:words', 10, { field: 'score' });
    // Longest-ever-word table ranked by length.
    var longest   = Storage.highscores('wordspire:longest', 10, { field: 'length' });

    function hs(mode) {
        if (mode === 'timed')  return hsTimed;
        if (mode === 'puzzle') return hsPuzzle;
        return hsClassic;
    }

    return {
        settings: settings,
        save: function () { store.save(); },
        hs: hs,
        qualifies: function (mode, value) { return hs(mode).qualifies(value); },
        add: function (mode, entry) { return hs(mode).add(entry); },
        list: function (mode) { return hs(mode).list(); },

        addWord: function (entry) {
            topWords.add(entry);
            longest.add(entry);
        },
        topWords: function () { return topWords.list(); },
        longestWords: function () { return longest.list(); },

        difficultyLabel: function () {
            return ['Easy', 'Normal', 'Hard'][settings.difficulty] || 'Normal';
        }
    };
})();
