// scoring.js — pure-function scoring helpers.
// Exposes:
//   W.Scoring.LETTER_VALUES           - per-letter points (Scrabble-ish)
//   W.Scoring.letterValue(ch)         - points for a single letter
//   W.Scoring.lengthBonus(n)          - 3=>10, 4=>20, 5=>40, 6=>80, 7=>160, 8+=>320*2^(n-8)
//   W.Scoring.computeWordScore(word, tiles)
//       word:   string
//       tiles:  array of tile objects (or { mult: N } / null). If omitted,
//               treated as plain tiles with mult=1.
//       returns: integer score
'use strict';
var W = window.W = window.W || {};

W.Scoring = (function () {
    var LETTER_VALUES = {
        a: 1, b: 3, c: 3, d: 2, e: 1, f: 4, g: 2, h: 4,
        i: 1, j: 8, k: 5, l: 1, m: 3, n: 1, o: 1, p: 3,
        q: 10, r: 1, s: 1, t: 1, u: 1, v: 4, w: 4, x: 8,
        y: 4, z: 10
    };

    function letterValue(ch) {
        if (!ch) return 0;
        var v = LETTER_VALUES[String(ch).toLowerCase()];
        return v == null ? 0 : v;
    }

    function lengthBonus(n) {
        if (n < 3) return 0;
        if (n === 3) return 10;
        if (n === 4) return 20;
        if (n === 5) return 40;
        if (n === 6) return 80;
        if (n === 7) return 160;
        // 8+: exponential
        return 320 * Math.pow(2, n - 8);
    }

    // Compute the score of a word, taking per-tile multipliers into account.
    //   tiles: optional array of { letter, mult } (mult defaults to 1).
    //          If a tile is missing or null, mult defaults to 1.
    // Score model:
    //   Sum letter values (each scaled by the tile's multiplier),
    //   then add the length bonus (also scaled by the max tile multiplier
    //   found in the chain — encourages using gilded/jeweled tiles).
    function computeWordScore(word, tiles) {
        if (!word || word.length < 3) return 0;
        var sum = 0;
        var maxMult = 1;
        for (var i = 0; i < word.length; i++) {
            var ch = word.charAt(i).toLowerCase();
            var v  = letterValue(ch);
            var t  = tiles && tiles[i];
            var m  = (t && t.mult) ? t.mult : 1;
            if (m > maxMult) maxMult = m;
            sum += v * m;
        }
        var bonus = lengthBonus(word.length) * maxMult;
        return Math.floor(sum + bonus);
    }

    // Simple combo multiplier: N consecutive valid submissions
    //   => multiplier = 1 + 0.5 * (N - 1), clamped at 5.
    function comboMultiplier(streak) {
        if (streak <= 1) return 1;
        var m = 1 + 0.5 * (streak - 1);
        return Math.min(5, m);
    }

    return {
        LETTER_VALUES: LETTER_VALUES,
        letterValue: letterValue,
        lengthBonus: lengthBonus,
        computeWordScore: computeWordScore,
        comboMultiplier: comboMultiplier
    };
})();
