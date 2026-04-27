// level.js — World 1-1 layout. ASCII rows; chars map to tile ids or entities.
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
// Grid: 60 cols × 18 rows of 32 px tiles → 1920 × 576 px world.

(function (global) {
    'use strict';

    const TILE_CHARS = {
        '.': 0, '#': 1, 'B': 2, 'Q': 3,
        '[': 4, ']': 5, '<': 6, '>': 7,
        'C': 8,
    };
    const SOLID_IDS = [1, 2, 3, 4, 5, 6, 7];

    const ENTITY_CHARS = { 'P': 'player', 'G': 'stomper', 'F': 'flag' };

    // 60 × 18.    012345678901234567890123456789012345678901234567890123456789
    const ROWS = [
        '............................................................', // 0
        '............................................................', // 1
        '....C.................C..............C......................', // 2
        '............................................................', // 3
        '............................................................', // 4
        '............................................................', // 5
        '...........BQB...................BBQBB......................', // 6
        '............................................................', // 7
        '............................................................', // 8
        '............................................................', // 9
        '............................................................', // 10
        '............................................................', // 11
        '............................................................', // 12
        '..............................[]............................', // 13
        '............G......G..........<>.....G......................', // 14
        '.....P........................<>...........................F', // 15
        '############..########.#######<>########....################', // 16
        '############..########.#######<>########....################', // 17
    ];

    const COLS = ROWS[0].length;
    const ROWS_N = ROWS.length;

    // Walks the grid once: produces the cleaned tile map (entities → empty)
    // and a list of entity spawn records in world pixels.
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
