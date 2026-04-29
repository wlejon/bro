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
//   I  beam pickup (granting the destructible-terrain weapon)
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
//   105–119 ground approach + beam pickup (col 115) + flag (col 118)

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
    const ENTITY_CHARS = {
        'P': 'player', 'G': 'stomper', 'F': 'flag', 'I': 'pickup',
        // Flying enemies. 'V' = horizontal patrol only; 'X' = horizontal +
        // sinusoidal vertical bob. Letters chosen so they don't collide
        // with existing tile chars (B, Q, [, ], <, >, C) or other entity
        // chars (P, G, F). 'X' rather than 'W' since 'W' is reserved as a
        // shorthand someone might want to add later for a wider gap glyph.
        'V': 'flyer', 'X': 'flyer_bob',
    };

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
        // Built from bricks rather than ground so it remains destructible —
        // ground tiles are pinned indestructible by the tilemap.
        fillCol(100, 15, 15, 'B');                  // 1 high
        fillCol(101, 14, 15, 'B');                  // 2 high
        fillCol(102, 13, 15, 'B');                  // 3 high
        fillCol(103, 12, 15, 'B');                  // 4 high
        fillCol(104, 11, 15, 'B');                  // 5 high

        // ── Flyers ──────────────────────────────────────────────────────────
        // Place flyers at three distinct heights so the agent has to learn
        // "when to jump" rather than "always jump" or "never jump":
        //
        //   row 11 (y=[352,368]): "NO-JUMP" zone. Overlaps player AABB at
        //       jump apex (player AABB at peak = [332,362]). Lethal if the
        //       agent jumps anywhere near one. Cannot be jumped over.
        //   row 12 (y=[384,400]): "TIMING" zone. Apex passes safely, but
        //       AABB overlap during ascent/descent — must time jumps so
        //       player passes through this row at apex (not transit).
        //       Used for bobbing flyers ('X') so height varies.
        //   row 15 (y=[480,496]): "FORCED-JUMP" zone. Body-height flyer
        //       overlaps the running player on ground — must jump over to
        //       proceed. Apex (332) clears flyer top (480) easily.
        //
        // 'V' = horizontal patrol; 'X' = horizontal + vertical bob.
        set(11,  6, 'V');   // intro: punish panic-jumps
        set(15, 20, 'V');   // post-gap-1: forced jump-over
        set(11, 33, 'V');   // pipes area: no jump-spam between pipes
        set(12, 41, 'X');   // post-pipes: bobbing timing
        set(11, 54, 'V');   // post-gap-2: lethal apex
        set(15, 60, 'V');   // mid-section: forced jump-over
        set(11, 68, 'V');   // pre-gap-3: lethal apex
        set(12, 76, 'X');   // gap-3 area: bobbing
        set(15, 86, 'V');   // long flat: forced jump-over
        set(11, 95, 'V');   // pre-staircase: must NOT jump
        set(12,108, 'X');   // post-staircase: bobbing late hazard

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
        set(15, 115, 'I');                          // beam pickup (gates destruction)
        set(15, 118, 'F');                          // flag

        return rows.map((arr) => arr.join(''));
    }

    const ROWS = buildRows();

    function load(opts) {
        const tileSize = (opts && opts.tileSize) || 32;
        const destructible = !!(opts && opts.destructible);
        const tm = Tilemap.create({
            tileSize, cols: COLS, rows: ROWS_N,
            drawTile: Art.drawTile,
            solidIds: SOLID_IDS,
            // Ground tile (id 1) is the floor — must stay intact so the player
            // can't blast through it. Bricks/pipes/Q-blocks are still fair game.
            indestructibleIds: [1],
            destructible,
            trackDamagedTiles: !(opts && opts.trackDamagedTiles === false),
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

    // Parse level entities into the per-mob template objects that
    // SwSim.create expects. All three workers (trainer / mcts / live)
    // and the live game share these conventions, so the construction
    // logic lives here.
    function makeStomper(e, tileSize) {
        return {
            x: e.x + 2,
            y: (e.row + 1) * tileSize - 24,
            w: 28, h: 24, vx: -50, vy: 0,
            onGround: false, alive: true, squashTimer: 0, animT: 0,
        };
    }
    function makeFlyer(e, tileSize) {
        const bob = e.kind === 'flyer_bob';
        const cx = e.col * tileSize + tileSize / 2;
        const cy = e.row * tileSize + tileSize / 2;
        const FLY_W = 24, FLY_H = 16;
        return {
            x: cx - FLY_W / 2, y: cy - FLY_H / 2,
            w: FLY_W, h: FLY_H, vx: -80, vy: 0,
            spawnX: cx - FLY_W / 2, spawnY: cy - FLY_H / 2,
            patrolRange: 96,
            bobAmp: bob ? 32 : 0, bobFreq: bob ? Math.PI : 0,
            bobT: 0, animT: 0, alive: true,
        };
    }
    function makeFlag(e, tileSize) {
        const flag = { x: e.x, w: 32, h: 96, y: e.row * tileSize - 64 };
        flag.y = e.row * tileSize - flag.h + tileSize;
        return flag;
    }
    function makePickup(e, tileSize) {
        // 24×24 collectible centered in its tile cell.
        const cx = e.col * tileSize + tileSize / 2;
        const cy = e.row * tileSize + tileSize / 2;
        return { x: cx - 12, y: cy - 12, w: 24, h: 24 };
    }

    // Build the {tilemap, spawn, stompers, flyers, flag, pickup} bundle
    // used by SwSim.create. opts forwards to load().
    function buildLevel(opts) {
        const tileSize = (opts && opts.tileSize) || 32;
        const lvl = load(opts);
        let spawn = { x: 0, y: 0 };
        const stompers = [];
        const flyers = [];
        let flag = null;
        let pickup = null;
        for (const e of lvl.entities) {
            if (e.kind === 'player') { spawn.x = e.x; spawn.y = e.y; }
            else if (e.kind === 'stomper') stompers.push(makeStomper(e, tileSize));
            else if (e.kind === 'flyer' || e.kind === 'flyer_bob')
                flyers.push(makeFlyer(e, tileSize));
            else if (e.kind === 'flag') flag = makeFlag(e, tileSize);
            else if (e.kind === 'pickup') pickup = makePickup(e, tileSize);
        }
        return {
            tilemap: lvl.tilemap, entities: lvl.entities,
            spawn, stompers, flyers, flag, pickup,
        };
    }

    global.Level = { load, buildLevel, makeStomper, makeFlyer, makeFlag, makePickup,
                     COLS, ROWS_N };
})(typeof window !== 'undefined' ? window : globalThis);
