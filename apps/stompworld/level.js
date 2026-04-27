// level.js — Stompworld Stage 1.
//
// Tile chars (solid):
//   #  ground          B  brick           Q  question
//   [  pipe top-left   ]  pipe top-right
//   <  pipe body-left  >  pipe body-right
// Tile chars (decorative, non-solid):
//   C  cloud           .  empty
// Entity chars (treated as empty tiles, then spawned):
//   P  player spawn    G  stomper         F  flag
//
// Grid: 120 cols × 18 rows of 32 px tiles → 3840 × 576 px world.
//
// Layout sections (left → right):
//   0–12   intro: solo Q-block, easy ground
//   13–15  3-tile gap
//   16–28  ground + first stomper + brick row
//   29–36  twin pipes (height 2 + height 3)
//   37–44  brick run with floating brick + stomper
//   45–49  5-tile gap with mid-air bounce brick
//   50–65  ground + 5-tile floating platform (QBBBQ) with stomper above
//   66–72  high 3-block bonus row + ground stomper
//   73–77  5-tile gap
//   78–95  long ground with mid-height brick cluster
//   96–104 5-step staircase ascent
//   105–119 ground approach + flag

(function (global) {
    'use strict';

    const COLS = 120;
    const ROWS_N = 18;

    const TILE_CHARS = {
        '.': 0, '#': 1, 'B': 2, 'Q': 3,
        '[': 4, ']': 5, '<': 6, '>': 7,
        'C': 8,
    };
    const SOLID_IDS = [1, 2, 3, 4, 5, 6, 7];
    const ENTITY_CHARS = { 'P': 'player', 'G': 'stomper', 'F': 'flag' };

    // Programmatic row builder: start with 120 dots, poke characters into columns.
    function buildRows() {
        const rows = new Array(ROWS_N);
        for (let r = 0; r < ROWS_N; r++) rows[r] = new Array(COLS).fill('.');
        function set(r, c, ch) { rows[r][c] = ch; }
        function setRange(r, c0, str) { for (let i = 0; i < str.length; i++) rows[r][c0 + i] = str[i]; }
        function fillCol(c, r0, r1, ch) { for (let r = r0; r <= r1; r++) rows[r][c] = ch; }

        // ── Sky decoration ───────────────────────────────────────────────────
        // Two parallax-friendly cloud bands (rows 1 + 3) staggered across world.
        [7, 25, 50, 78, 105].forEach((c) => set(1, c, 'C'));
        [15, 38, 65, 90, 112].forEach((c) => set(3, c, 'C'));

        // ── Ground (rows 16–17) with gaps ────────────────────────────────────
        // Solid runs: 0–12, 16–44, 50–72, 78–119
        // Gaps:        13–15, 45–49, 73–77
        const GROUND = [
            [0, 12], [16, 44], [50, 72], [78, 119],
        ];
        for (const [a, b] of GROUND) {
            for (let c = a; c <= b; c++) { set(16, c, '#'); set(17, c, '#'); }
        }

        // ── Pipes ────────────────────────────────────────────────────────────
        // Pipe 1: cols 30–31, height 2  (top row 14, body row 15)
        setRange(14, 30, '[]'); setRange(15, 30, '<>');
        // Pipe 2: cols 35–36, height 3  (top row 13, body rows 14–15)
        setRange(13, 35, '[]'); setRange(14, 35, '<>'); setRange(15, 35, '<>');

        // ── Question / brick clusters ────────────────────────────────────────
        set(12, 8, 'Q');                            // tutorial bonus
        setRange(12, 22, 'BQB');                    // mid-cluster
        set(12, 40, 'B');                           // floating brick (reachable from ground)
        set(12, 47, 'B');                           // mid-air bounce brick over first 5-tile gap
        setRange(12, 56, 'QBBBQ');                  // 5-tile floating platform (reachable from ground)
        setRange(9,  70, 'BQB');                    // high bonus row (reach via QBBBQ → jump)
        set(12, 75, 'B');                           // stepping brick over second 5-tile gap
        setRange(8,  84, 'BBQBB');                  // upper cluster (decorative skyline)

        // ── Final staircase (cols 100–104, rising right) ────────────────────
        // Each column is solid from a given row down to row 15.
        fillCol(100, 15, 15, '#');                  // 1 high
        fillCol(101, 14, 15, '#');                  // 2 high
        fillCol(102, 13, 15, '#');                  // 3 high
        fillCol(103, 12, 15, '#');                  // 4 high
        fillCol(104, 11, 15, '#');                  // 5 high

        // ── Spawns ──────────────────────────────────────────────────────────
        set(15, 2, 'P');                            // player
        set(15, 17, 'G');                           // tutorial stomper
        set(15, 28, 'G');                           // pre-pipe stomper
        set(15, 42, 'G');                           // post-pipe stomper
        set(15, 53, 'G');                           // mid-section ground stomper
        set(11, 58, 'G');                           // stomper standing on floating platform
        set(15, 62, 'G');                           // ground stomper under platform
        set(15, 80, 'G');                           // long-ground stomper
        set(15, 90, 'G');                           // long-ground stomper
        set(15, 118, 'F');                          // flag

        return rows.map((arr) => arr.join(''));
    }

    const ROWS = buildRows();

    function load(opts) {
        const tileSize = (opts && opts.tileSize) || 32;
        const tm = Tilemap.create({
            tileSize, cols: COLS, rows: ROWS_N,
            drawTile: Art.drawTile,
            solidIds: SOLID_IDS,
        });

        const entities = [];
        const cleaned = ROWS.map((row, r) => {
            let out = '';
            for (let c = 0; c < row.length; c++) {
                const ch = row[c];
                if (ENTITY_CHARS[ch]) {
                    entities.push({
                        kind: ENTITY_CHARS[ch],
                        col: c, row: r,
                        x: c * tileSize, y: r * tileSize,
                    });
                    out += '.';
                } else {
                    out += ch;
                }
            }
            return out;
        });
        tm.setRows(cleaned, TILE_CHARS);

        return { tilemap: tm, entities };
    }

    global.Level = { load, COLS, ROWS_N };
})(typeof window !== 'undefined' ? window : globalThis);
