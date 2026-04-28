// art.js — code-driven pixel art baked once into offscreen canvases at boot,
// then blitted via drawImage each frame. Recipes are ASCII grids + small
// palettes; brush.stamp paints them into the atlas/sprite canvases. Same
// idea as artstation/brush.stamp, kept inline so the app has no external
// asset dependency.
//
// We rely on bro's CanvasRenderingContext2D.drawImage accepting offscreen
// HTMLCanvasElement as a source — exercised here because Stompworld is the
// reference implementation for the new sprite/tilemap stack.

(function (global) {
    'use strict';

    const TILE = 32;
    const N    = 8;            // 8×8 source cells per tile
    const TPIX = TILE / N;     // 4 px per cell

    function makeCanvas(w, h) {
        const c = document.createElement('canvas');
        c.width = w; c.height = h;
        const cx = c.getContext('2d');
        cx.imageSmoothingEnabled = false;
        return c;
    }

    function stamp(ctx, sx, sy, rows, palette, cell) {
        for (let r = 0; r < rows.length; r++) {
            const s = rows[r];
            for (let c = 0; c < s.length; c++) {
                const col = palette[s[c]];
                if (!col) continue;
                ctx.fillStyle = col;
                ctx.fillRect(sx + c * cell, sy + r * cell, cell, cell);
            }
        }
    }

    // ── Tile atlas (single canvas, 9 tiles wide for ids 1..8) ───────────────
    const TILE_PAL = {
        G: '#5cd14a', g: '#3aa42a', d: '#9b5a2a', D: '#7a3d1c',
        m: '#5a1a08', B: '#c95a28',
        k: '#5a3608', H: '#ffcf3a',
        P: '#114414', p: '#3a9b3e', V: '#1f6f25',
        W: '#ffffff',
        '.': null,
    };
    const TILE_RECIPES = {
        1: [ // ground
            'GGGGGGGG', 'gGgGGgGg',
            'dDdDdDdD', 'DdDdDdDd',
            'dDdDdDdD', 'DdDdDdDd',
            'dDdDdDdD', 'DdDdDdDd',
        ],
        2: [ // brick
            'mmmmmmmm', 'BBBBBBBB',
            'BBBmBBBm', 'BBBmBBBm',
            'mmmmmmmm', 'mBBBmBBB',
            'mBBBmBBB', 'mBBBmBBB',
        ],
        3: [ // question block
            'kkkkkkkk', 'kHHHHHHk',
            'kHHkkHHk', 'kHHkkHHk',
            'kHHHkHHk', 'kHHkHHHk',
            'kHHkHHHk', 'kkkkkkkk',
        ],
        4: [ // pipe top-left
            '..PPPPPP', '.PPpppVV',
            'PppVVVVV', 'PpVVVVVV',
            'PpVVVVVV', 'PpVVVVVV',
            'PpVVVVVV', 'PpVVVVVV',
        ],
        5: [ // pipe top-right
            'PPPPPP..', 'VVpppPP.',
            'VVVVVppP', 'VVVVVVpP',
            'VVVVVVpP', 'VVVVVVpP',
            'VVVVVVpP', 'VVVVVVpP',
        ],
        6: [ // pipe body left
            'PpVVVVVV', 'PpVVVVVV',
            'PpVVVVVV', 'PpVVVVVV',
            'PpVVVVVV', 'PpVVVVVV',
            'PpVVVVVV', 'PpVVVVVV',
        ],
        7: [ // pipe body right
            'VVVVVVpP', 'VVVVVVpP',
            'VVVVVVpP', 'VVVVVVpP',
            'VVVVVVpP', 'VVVVVVpP',
            'VVVVVVpP', 'VVVVVVpP',
        ],
        8: [ // cloud (decorative, non-solid)
            '...WW...', '..WWWW..',
            '.WWWWWW.', 'WWWWWWWW',
            'WWWWWWWW', '.WWWWWW.',
            '...WW...', '........',
        ],
    };

    let atlas = null;
    const ATLAS_COLS = 8;

    function buildAtlas() {
        atlas = makeCanvas(ATLAS_COLS * TILE, TILE);
        const cx = atlas.getContext('2d');
        for (const id in TILE_RECIPES) {
            const sx = ((+id - 1) % ATLAS_COLS) * TILE;
            stamp(cx, sx, 0, TILE_RECIPES[id], TILE_PAL, TPIX);
        }
    }

    function drawTile(ctx, id, x, y, size) {
        if (!atlas) buildAtlas();
        if (!TILE_RECIPES[id]) return;
        const sx = ((id - 1) % ATLAS_COLS) * TILE;
        ctx.drawImage(atlas, sx, 0, TILE, TILE, x, y, size || TILE, size || TILE);
    }

    // ── Hero sheet (4 frames × 24×32) ────────────────────────────────────────
    const HERO_PAL = {
        S: '#1a3055', R: '#e83b3b', s: '#f4c890', h: '#3a1d10', B: '#000',
        '.': null,
    };
    const HERO_FRAMES = [
        // 0 idle
        ['.BBBB.', 'BhhhhB', 'BhsshB', 'BsshsB',
         '.SRRS.', 'SSRRSS', 'SS..SS', 'hh..hh'],
        // 1 run-a
        ['.BBBB.', 'BhhhhB', 'BhsshB', 'BssSsB',
         '.SRRS.', 'SSRRSS', '.S..S.', 'hh..hh'],
        // 2 run-b
        ['.BBBB.', 'BhhhhB', 'BhsshB', 'BssSsB',
         '.SRRS.', 'SSRRSS', 'SS....', 'hh..hh'],
        // 3 jump
        ['.BBBB.', 'BhhhhB', 'BhsshB', 'BssSsB',
         'SSRRSS', 'SSRRSS', 'SS..SS', '.h..h.'],
    ];
    const HERO_W = 24, HERO_H = 32, HERO_CELL = 4;

    let heroSheet = null;
    function buildHero() {
        heroSheet = makeCanvas(HERO_W * HERO_FRAMES.length, HERO_H);
        const cx = heroSheet.getContext('2d');
        for (let i = 0; i < HERO_FRAMES.length; i++) {
            stamp(cx, i * HERO_W, 0, HERO_FRAMES[i], HERO_PAL, HERO_CELL);
        }
    }

    function drawHero(ctx, x, y, frame, flipX) {
        if (!heroSheet) buildHero();
        const sx = (frame || 0) * HERO_W;
        if (flipX) {
            ctx.save();
            ctx.translate(x + HERO_W, y);
            ctx.scale(-1, 1);
            ctx.drawImage(heroSheet, sx, 0, HERO_W, HERO_H, 0, 0, HERO_W, HERO_H);
            ctx.restore();
        } else {
            ctx.drawImage(heroSheet, sx, 0, HERO_W, HERO_H, x, y, HERO_W, HERO_H);
        }
    }

    // ── Stomper sheet (3 frames × 28×24) ─────────────────────────────────────
    // Brighter body palette so the silhouette reads at game distance.
    const STOMP_PAL = {
        B: '#000', b: '#9a4a18', t: '#d27b32',
        w: '#fff', p: '#222', f: '#5a2210',
        '.': null,
    };
    const STOMP_FRAMES = [
        // 0 walk-a
        ['.BBBBB.', 'BbttttB', 'BtwpwBB',
         'BtwpwbB', 'BbtttbB', 'ffBBff.'],
        // 1 walk-b
        ['.BBBBB.', 'BbttttB', 'BtwpwBB',
         'BtwpwbB', 'BbtttbB', '.ffBBff'],
        // 2 squashed (low silhouette, body anchored to bottom of frame)
        ['.......', '.......', '.......',
         '.BBBBB.', 'BbtwpwB', 'fffffff'],
    ];
    const STOMP_W = 28, STOMP_H = 24, STOMP_CELL = 4;

    let stompSheet = null;
    function buildStomper() {
        stompSheet = makeCanvas(STOMP_W * STOMP_FRAMES.length, STOMP_H);
        const cx = stompSheet.getContext('2d');
        for (let i = 0; i < STOMP_FRAMES.length; i++) {
            stamp(cx, i * STOMP_W, 0, STOMP_FRAMES[i], STOMP_PAL, STOMP_CELL);
        }
    }

    function drawStomper(ctx, x, y, frame) {
        if (!stompSheet) buildStomper();
        const sx = (frame || 0) * STOMP_W;
        ctx.drawImage(stompSheet, sx, 0, STOMP_W, STOMP_H, x, y, STOMP_W, STOMP_H);
    }

    // ── Flyer sheet (2 frames × 24×16) ───────────────────────────────────────
    // A flapping bat-ish silhouette: small, dark, easy to read against sky.
    // Two frames so wings animate in the live game.
    const FLY_PAL = {
        B: '#000', d: '#3a1a4a', p: '#7a3aaa', e: '#ff6655',
        '.': null,
    };
    const FLY_FRAMES = [
        // 0 wings up
        ['BB....BB', 'BdB..BdB', 'BddBBddB',
         '.BdpdpdB', '.BddedB.', '..BBBB..'],
        // 1 wings down
        ['........', 'BB....BB', 'BdBBBBdB',
         'BdpdpdpB', 'BBddedBB', '..BBBB..'],
    ];
    const FLY_W = 24, FLY_H = 16, FLY_CELL = 3;
    let flyerSheet = null;
    function buildFlyer() {
        flyerSheet = makeCanvas(FLY_W * FLY_FRAMES.length, FLY_H);
        const cx = flyerSheet.getContext('2d');
        for (let i = 0; i < FLY_FRAMES.length; i++) {
            stamp(cx, i * FLY_W, 0, FLY_FRAMES[i], FLY_PAL, FLY_CELL);
        }
    }
    function drawFlyer(ctx, x, y, frame, flipX) {
        if (!flyerSheet) buildFlyer();
        const sx = (frame || 0) * FLY_W;
        if (flipX) {
            ctx.save();
            ctx.translate(x + FLY_W, y);
            ctx.scale(-1, 1);
            ctx.drawImage(flyerSheet, sx, 0, FLY_W, FLY_H, 0, 0, FLY_W, FLY_H);
            ctx.restore();
        } else {
            ctx.drawImage(flyerSheet, sx, 0, FLY_W, FLY_H, x, y, FLY_W, FLY_H);
        }
    }

    // ── Flag (32×96, baked once) ─────────────────────────────────────────────
    let flagCanvas = null;
    function buildFlag() {
        flagCanvas = makeCanvas(32, 96);
        const cx = flagCanvas.getContext('2d');
        // pole
        cx.fillStyle = '#cfd6e0'; cx.fillRect(14, 0, 4, 96);
        // ball top
        cx.fillStyle = '#ffd84d'; cx.fillRect(10, 0, 12, 6); cx.fillRect(12, 6, 8, 4);
        // triangular flag
        cx.fillStyle = '#e83b3b';
        cx.beginPath();
        cx.moveTo(18, 10); cx.lineTo(30, 18); cx.lineTo(18, 26); cx.closePath();
        cx.fill();
    }

    function drawFlag(ctx, x, y) {
        if (!flagCanvas) buildFlag();
        ctx.drawImage(flagCanvas, x, y);
    }

    // Beam pickup: chunky 24×24 yellow-rimmed canister with a slow bob.
    // x, y are top-left in world space; t is an animation phase (ms).
    function drawPickup(ctx, x, y, t) {
        const off = Math.sin((t || 0) * 0.005) * 2;
        ctx.save();
        ctx.translate(x, y + off);
        ctx.fillStyle = '#222';
        ctx.fillRect(0, 0, 24, 24);
        ctx.fillStyle = '#ffd84d';
        ctx.fillRect(2, 2, 20, 20);
        ctx.fillStyle = '#e83b3b';
        ctx.fillRect(6, 6, 12, 12);
        ctx.fillStyle = '#fff';
        ctx.fillRect(10, 4, 4, 16);
        ctx.fillRect(4, 10, 16, 4);
        ctx.restore();
    }

    global.Art = {
        TILE,
        HERO_W, HERO_H,
        STOMP_W, STOMP_H,
        FLY_W, FLY_H,
        drawTile, drawHero, drawStomper, drawFlyer, drawFlag, drawPickup,
    };
})(typeof window !== 'undefined' ? window : globalThis);
