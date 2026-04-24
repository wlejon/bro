// puzzles.js — Puzzle mode layouts. 'F' prefix = frozen cell.
// Each puzzle is an 8-row 8-col array of strings. Empty string = random fill.
'use strict';
var G = G || {};

G.Puzzles = (function () {
    // Helper to generate a puzzle with a random pattern of frozen cells.
    function gen(seed, density) {
        var rows = 8, cols = 8;
        var out = [];
        // tiny LCG
        var s = seed;
        function rand() { s = (s * 1103515245 + 12345) & 0x7fffffff; return (s >>> 8) / 0xffffff; }
        for (var r = 0; r < rows; r++) {
            var row = [];
            for (var c = 0; c < cols; c++) {
                var color = 1 + Math.floor(rand() * 7);
                if (rand() < density) row.push('F' + color);
                else row.push(String(color));
            }
            out.push(row);
        }
        return out;
    }

    // Hand-seeded patterns with frozen tiles in evocative layouts.
    var P = [
        // 1 — edges
        [
            ['F1','F2','F3','F4','F5','F6','F7','F1'],
            ['F2','','','','','','','F2'],
            ['F3','','','','','','','F3'],
            ['F4','','','','','','','F4'],
            ['F5','','','','','','','F5'],
            ['F6','','','','','','','F6'],
            ['F7','','','','','','','F7'],
            ['F1','F2','F3','F4','F5','F6','F7','F1'],
        ],
        // 2 — diagonal stripe
        [
            ['F1','','','','','','',''],
            ['','F2','','','','','',''],
            ['','','F3','','','','',''],
            ['','','','F4','','','',''],
            ['','','','','F5','','',''],
            ['','','','','','F6','',''],
            ['','','','','','','F7',''],
            ['','','','','','','','F1'],
        ],
        // 3 — checker
        [
            ['F1','','F2','','F3','','F4',''],
            ['','F5','','F6','','F7','','F1'],
            ['F2','','F3','','F4','','F5',''],
            ['','F6','','F7','','F1','','F2'],
            ['F3','','F4','','F5','','F6',''],
            ['','F7','','F1','','F2','','F3'],
            ['F4','','F5','','F6','','F7',''],
            ['','F1','','F2','','F3','','F4'],
        ],
        // 4 — middle box
        [
            ['','','','','','','',''],
            ['','','','','','','',''],
            ['','','F1','F2','F3','F4','',''],
            ['','','F2','','','F5','',''],
            ['','','F3','','','F6','',''],
            ['','','F4','F5','F6','F7','',''],
            ['','','','','','','',''],
            ['','','','','','','',''],
        ],
        // 5 — corners
        [
            ['F1','F2','','','','','F3','F4'],
            ['F2','','','','','','','F5'],
            ['','','','','','','',''],
            ['','','','','','','',''],
            ['','','','','','','',''],
            ['','','','','','','',''],
            ['F6','','','','','','','F7'],
            ['F7','F1','','','','','F2','F3'],
        ],
        // 6 — plus sign
        [
            ['','','','F1','F2','','',''],
            ['','','','F3','F4','','',''],
            ['','','','F5','F6','','',''],
            ['F1','F2','F3','F4','F5','F6','F7','F1'],
            ['F2','F3','F4','F5','F6','F7','F1','F2'],
            ['','','','F7','F1','','',''],
            ['','','','F2','F3','','',''],
            ['','','','F4','F5','','',''],
        ],
        // 7-20: procedural variants.
    ];

    for (var i = P.length; i < 20; i++) {
        P.push(gen(1000 + i, 0.15 + (i % 4) * 0.05));
    }

    return {
        count: function () { return P.length; },
        get: function (i) { return P[i % P.length]; },
    };
})();
