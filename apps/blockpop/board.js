// board.js — Blockpop board: rising columns, carrier, pop logic, specials.
// Convention: board is a 2D array of column-major stacks. board[col] is an
// array of blocks from BOTTOM (index 0) to TOP. A block is null for empty
// or { color, special } where color is 1..NUM_COLORS and special is one
// of SPECIAL_* constants.
'use strict';
var G = window.G = window.G || {};

G.Board = (function () {
    var COLS = 8;
    var ROWS = 16;
    var NUM_COLORS = 7;
    var SPECIAL_NONE = 0;
    var SPECIAL_STAR = 1;
    var SPECIAL_BOMB = 2;
    var SPECIAL_RAINBOW = 3;

    // Color palette + paired "shape" for colorblind glyphs.
    var COLORS = [null,
        '#ff4d6d', // red
        '#ffb74d', // orange
        '#ffeb3b', // yellow
        '#66e676', // green
        '#4ad6ff', // cyan
        '#6b8cff', // blue
        '#c47bff'  // purple
    ];
    var SHAPES = [null, 'circle', 'square', 'triangle', 'diamond', 'hex', 'plus', 'star'];

    // Simple deterministic RNG (mulberry32) for seeded unit tests.
    function makeRng(seed) {
        var s = (seed >>> 0) || 1;
        return function () {
            s |= 0; s = (s + 0x6D2B79F5) | 0;
            var t = Math.imul(s ^ (s >>> 15), 1 | s);
            t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
            return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
        };
    }
    var rng = Math.random;

    // ---- Pure helpers -------------------------------------------------------

    function makeEmptyBoard() {
        var b = [];
        for (var c = 0; c < COLS; c++) b.push([]);
        return b;
    }

    function columnHeight(board, c) { return board[c].length; }

    function blockAt(board, c, r) {
        // r counts from bottom: 0 is bottom-most block.
        if (c < 0 || c >= COLS) return null;
        if (r < 0 || r >= board[c].length) return null;
        return board[c][r];
    }

    function cloneBoard(board) {
        var out = [];
        for (var c = 0; c < COLS; c++) {
            var col = [];
            for (var r = 0; r < board[c].length; r++) {
                var b = board[c][r];
                col.push(b ? { color: b.color, special: b.special } : null);
            }
            out.push(col);
        }
        return out;
    }

    // Color-compat check: rainbow matches anything.
    function colorsMatch(a, b) {
        if (!a || !b) return false;
        if (a.special === SPECIAL_RAINBOW || b.special === SPECIAL_RAINBOW) return true;
        return a.color === b.color;
    }

    // Find connected components of matching colored blocks that include a
    // column's TOP block. The game pops blocks where the flood-fill from
    // any column's topmost cell contains 3+ cells (classic rising-stack
    // rule: only the exposed tops chain). We also allow connected 4-neighbour
    // components of same color anywhere in the stack if they include at
    // least one top.
    function findChains(board) {
        var visited = {};
        var groups = [];
        // Seed: top block of each column.
        for (var c = 0; c < COLS; c++) {
            var h = board[c].length;
            if (h === 0) continue;
            var r = h - 1;
            var key = c + ',' + r;
            if (visited[key]) continue;
            var top = board[c][r];
            if (!top) continue;
            // Flood-fill across tops: neighbors are the TOP blocks of adjacent
            // columns only (classic top-row matching), plus cells directly
            // below the seed if color matches (stack chain within col).
            var stack = [[c, r]];
            var cells = [];
            var seedColor = top.color;
            var seedIsRainbow = top.special === SPECIAL_RAINBOW;
            while (stack.length) {
                var n = stack.pop();
                var cc = n[0], rr = n[1];
                var k = cc + ',' + rr;
                if (visited[k]) continue;
                var bl = blockAt(board, cc, rr);
                if (!bl) continue;
                var matches =
                    seedIsRainbow
                        ? true
                        : (bl.color === seedColor || bl.special === SPECIAL_RAINBOW);
                if (!matches) continue;
                visited[k] = true;
                cells.push([cc, rr]);
                // Adjacent TOP blocks of neighbor columns.
                for (var d = -1; d <= 1; d += 2) {
                    var nc = cc + d;
                    if (nc < 0 || nc >= COLS) continue;
                    var nh = board[nc].length;
                    if (nh === 0) continue;
                    var nr = nh - 1;
                    stack.push([nc, nr]);
                }
                // Block directly below within same column (classic runs of
                // like-color stacking).
                if (rr - 1 >= 0) stack.push([cc, rr - 1]);
                // Block directly above (in case we reached from neighbor top).
                if (rr + 1 < board[cc].length) stack.push([cc, rr + 1]);
            }
            if (cells.length >= 3) {
                groups.push({ cells: cells, color: seedColor });
            }
        }
        return groups;
    }

    // Expand a group with special-block effects: star pops everything of
    // landing color; bomb adds 3x3 around each bomb cell; rainbow counts
    // as matching, no extra spread beyond flood-fill.
    function expandSpecials(board, group) {
        var set = {};
        for (var i = 0; i < group.cells.length; i++) {
            set[group.cells[i][0] + ',' + group.cells[i][1]] = true;
        }

        function addAll(list) { for (var i = 0; i < list.length; i++) set[list[i]] = true; }

        // Check for star/bomb within the group's cells.
        for (var i = 0; i < group.cells.length; i++) {
            var c = group.cells[i][0], r = group.cells[i][1];
            var b = blockAt(board, c, r);
            if (!b) continue;
            if (b.special === SPECIAL_STAR) {
                // Add all blocks of the group's color across the board.
                for (var cc = 0; cc < COLS; cc++) {
                    for (var rr = 0; rr < board[cc].length; rr++) {
                        var bb = board[cc][rr];
                        if (bb && bb.color === group.color) {
                            set[cc + ',' + rr] = true;
                        }
                    }
                }
            } else if (b.special === SPECIAL_BOMB) {
                // 3x3 around (c,r).
                var list = [];
                for (var dc = -1; dc <= 1; dc++) {
                    for (var dr = -1; dr <= 1; dr++) {
                        var nc = c + dc, nr = r + dr;
                        if (nc < 0 || nc >= COLS) continue;
                        if (nr < 0 || nr >= board[nc].length) continue;
                        list.push(nc + ',' + nr);
                    }
                }
                addAll(list);
            }
        }

        // Flatten back to cell list.
        var out = [];
        for (var k in set) {
            var parts = k.split(','); out.push([+parts[0], +parts[1]]);
        }
        return { cells: out, color: group.color };
    }

    // Remove popped cells from the board and settle gaps within columns.
    function popChains(board, groups) {
        var removed = 0;
        var perColRemoved = {};
        var popInfo = [];
        for (var g = 0; g < groups.length; g++) {
            var ex = expandSpecials(board, groups[g]);
            popInfo.push(ex);
            for (var i = 0; i < ex.cells.length; i++) {
                var c = ex.cells[i][0], r = ex.cells[i][1];
                if (board[c][r]) {
                    board[c][r] = null;
                    removed++;
                    perColRemoved[c] = (perColRemoved[c] || 0) + 1;
                }
            }
        }
        // Compact columns.
        for (var c2 = 0; c2 < COLS; c2++) {
            var kept = [];
            for (var r2 = 0; r2 < board[c2].length; r2++) {
                if (board[c2][r2]) kept.push(board[c2][r2]);
            }
            board[c2] = kept;
        }
        return { removed: removed, groups: popInfo };
    }

    // Alias: settle columns (compact nulls). Exposed for tests.
    function settle(board) {
        for (var c = 0; c < COLS; c++) {
            var kept = [];
            for (var r = 0; r < board[c].length; r++) {
                if (board[c][r]) kept.push(board[c][r]);
            }
            board[c] = kept;
        }
        return board;
    }

    // Spawn a new bottom row by pushing one block into each column (at
    // index 0). Occasionally inject a special block.
    function spawnRow(board, rngFn) {
        var r = rngFn || rng;
        for (var c = 0; c < COLS; c++) {
            var color = 1 + Math.floor(r() * NUM_COLORS);
            var special = SPECIAL_NONE;
            var roll = r();
            if (roll < 0.008) special = SPECIAL_STAR;
            else if (roll < 0.018) special = SPECIAL_BOMB;
            else if (roll < 0.025) special = SPECIAL_RAINBOW;
            // Avoid immediate stacked match of 3: if bottom two are same color,
            // pick another color for this spawn.
            if (board[c].length >= 2) {
                var b0 = board[c][0], b1 = board[c][1];
                if (b0 && b1 && b0.color === color && b1.color === color) {
                    color = 1 + ((color) % NUM_COLORS);
                }
            }
            board[c].unshift({ color: color, special: special });
        }
        return board;
    }

    function seedBoard(rows, rngFn) {
        var r = rngFn || rng;
        var b = makeEmptyBoard();
        for (var i = 0; i < rows; i++) spawnRow(b, r);
        // Remove any starting matches.
        var guard = 12;
        while (guard-- > 0) {
            var g = findChains(b);
            if (!g.length) break;
            popChains(b, g);
            settle(b);
            // Refill to same total height after removals.
            var shortest = rows;
            for (var c = 0; c < COLS; c++) shortest = Math.min(shortest, b[c].length);
            while (shortest < rows) { spawnRow(b, r); shortest++; }
        }
        return b;
    }

    // ---- Game state ---------------------------------------------------------

    var board = makeEmptyBoard();
    var carrier = { col: 3, heldStack: [] }; // up to HOLD_MAX blocks
    var HOLD_MAX = 3;
    var mode = 'classic';
    var score = 0;
    var level = 1;
    var chainDepth = 0;
    var maxChain = 0;
    var blocksPopped = 0;
    var bestChain = 0;
    var gameTime = 0;
    var finished = false;
    var gameOver = false;

    // Rising: progressPx = 0..CELL; when it reaches CELL, spawn a row.
    var riseProgress = 0;
    var riseSpeedPx = 8; // px/sec base (scaled per level)
    var nextRiseAccum = 0; // for HUD display in seconds

    // Sprint target
    var sprintTarget = 100;

    // Puzzle state
    var puzzleIdx = 0;
    var puzzleMoves = 0;
    var puzzleMovesLeft = 0;

    // Emergency brake
    var brakeActive = 0;   // remaining ms of slow
    var brakeCooldown = 0; // ms until usable again

    // Layout (computed in calcLayout).
    var layout = {
        cell: 40, ox: 80, oy: 80, w: 320, h: 640, topPad: 60
    };

    // Animations: individual settling/animating blocks when the carrier
    // drops. For simplicity we re-resolve on each drop (no tweens for
    // gameplay but tweens for popped particles).
    var popAnimTimer = 0;

    // Seeded RNG for deterministic tests via setSeed.
    var gameRng = rng;
    function setSeed(s) { gameRng = makeRng(s); rng = gameRng; }

    // ---- Public gameplay API ------------------------------------------------

    function startGame(m, opts) {
        opts = opts || {};
        mode = m || 'classic';
        score = 0;
        level = 1;
        chainDepth = 0;
        maxChain = 0;
        blocksPopped = 0;
        bestChain = 0;
        gameTime = 0;
        finished = false;
        gameOver = false;
        carrier.col = Math.floor(COLS / 2) - 1;
        carrier.heldStack = [];
        riseProgress = 0;
        brakeActive = 0;
        brakeCooldown = 0;
        nextRiseAccum = 0;

        if (mode === 'puzzle') {
            puzzleIdx = opts.puzzleIdx || 0;
            loadPuzzle(puzzleIdx);
        } else if (mode === 'sprint') {
            sprintTarget = 100;
            board = seedBoard(6, gameRng);
        } else {
            board = seedBoard(5, gameRng);
        }

        G.Particles.clear();
        updateHUD();
    }

    // Procedurally build a puzzle from a numeric idx. Each puzzle seeds a
    // board with known hand-pickable structures.
    function loadPuzzle(idx) {
        var r = makeRng(1000 + idx * 17);
        board = makeEmptyBoard();
        var rows = 6 + (idx % 5);
        for (var i = 0; i < rows; i++) spawnRow(board, r);
        // Insert 1-2 specials near the top to give the puzzle flavor.
        var specials = [SPECIAL_STAR, SPECIAL_BOMB, SPECIAL_RAINBOW];
        var cnt = 1 + (idx % 3);
        for (var s = 0; s < cnt; s++) {
            var c = Math.floor(r() * COLS);
            var rr = board[c].length - 1;
            if (rr >= 0) board[c][rr].special = specials[(idx + s) % specials.length];
        }
        // Give a move budget based on difficulty.
        puzzleMoves = 8 + idx;
        puzzleMovesLeft = puzzleMoves;
    }

    function calcLayout(W, H) {
        var topPad = 80;
        var bottomPad = 40;
        var avail = H - topPad - bottomPad;
        var cellByHeight = Math.floor(avail / ROWS);
        var cellByWidth = Math.floor((W - 80) / COLS);
        var cell = Math.min(cellByHeight, cellByWidth);
        if (cell < 14) cell = 14;
        layout.cell = cell;
        layout.w = cell * COLS;
        layout.h = cell * ROWS;
        layout.ox = Math.floor((W - layout.w) / 2);
        layout.oy = topPad;
        layout.topPad = topPad;
    }

    // Carrier movement
    function moveTo(col) {
        if (col < 0) col = 0;
        if (col >= COLS) col = COLS - 1;
        if (col === carrier.col) return;
        carrier.col = col;
        G.Audio.move();
    }
    function moveLeft()  { moveTo(carrier.col - 1); }
    function moveRight() { moveTo(carrier.col + 1); }

    function canPlace() {
        if (!carrier.heldStack.length) return false;
        if (board[carrier.col].length >= ROWS) return false;
        return true;
    }

    // Pick topmost block of current column into the held stack (top-front).
    function pick() {
        if (carrier.heldStack.length >= HOLD_MAX) return false;
        var col = board[carrier.col];
        if (!col.length) return false;
        var b = col.pop();
        carrier.heldStack.push(b);
        G.Audio.pick();
        return true;
    }

    // Place the front-most (last) held block onto the current column.
    function place() {
        if (!carrier.heldStack.length) return false;
        if (board[carrier.col].length >= ROWS) return false;
        var b = carrier.heldStack.pop();
        board[carrier.col].push(b);
        G.Audio.drop();
        if (mode === 'puzzle') puzzleMovesLeft--;
        resolveChains();
        return true;
    }

    // Pick-or-drop depending on whether we hold a block.
    function interact() {
        if (carrier.heldStack.length > 0) place();
        else pick();
    }

    function shuffleHeld() {
        if (carrier.heldStack.length < 2) return;
        carrier.heldStack.reverse();
        G.Audio.shuffle();
    }

    function emergencyBrake() {
        if (brakeCooldown > 0) return;
        brakeActive = 3000;
        brakeCooldown = 10000;
        G.Audio.brake();
        G.Particles.showAction('BRAKE');
    }

    // ---- Chain resolution ---------------------------------------------------

    // Called after every placement; also after specials. Runs cascade loop
    // until no more pops.
    function resolveChains() {
        var depth = 0;
        while (true) {
            var groups = findChains(board);
            if (!groups.length) break;
            depth++;
            chainDepth = depth;
            if (depth > bestChain) bestChain = depth;
            // Score
            var popped = 0;
            var specialKinds = [];
            for (var g = 0; g < groups.length; g++) {
                var ex = expandSpecials(board, groups[g]);
                var n = ex.cells.length;
                popped += n;
                // Base per-group
                var base = 150;
                if (n === 4) base = 300;
                else if (n === 5) base = 600;
                else if (n >= 6) base = 600 + (n - 5) * 250;
                score += base * depth;

                // Spawn particles per cell + emit specials pop
                for (var ci = 0; ci < ex.cells.length; ci++) {
                    var c = ex.cells[ci][0], r = ex.cells[ci][1];
                    var bl = board[c][r];
                    if (bl) {
                        var sh = bl.special;
                        if (sh === SPECIAL_STAR) specialKinds.push('star');
                        else if (sh === SPECIAL_BOMB) specialKinds.push('bomb');
                        else if (sh === SPECIAL_RAINBOW) specialKinds.push('rainbow');
                    }
                    var px = layout.ox + c * layout.cell + layout.cell / 2;
                    var py = layout.oy + layout.h - (r + 0.5) * layout.cell;
                    var col = COLORS[ex.color] || '#fff';
                    G.Particles.spawn(px, py, 6, col, {
                        spread: 5, spreadY: 4, life: 500, lifeVar: 400, size: 3, sizeVar: 2
                    });
                    G.Particles.flash(
                        layout.ox + c * layout.cell,
                        layout.oy + layout.h - (r + 1) * layout.cell,
                        layout.cell, layout.cell, '#ffffff', 200);
                }
            }
            G.Audio.pop(groups[0].color, depth - 1);
            for (var sk = 0; sk < specialKinds.length; sk++) G.Audio.special(specialKinds[sk]);

            // Cascade bonus text
            if (depth >= 2) {
                G.Particles.showCascade('x' + depth + ' CHAIN');
                if (depth > maxChain) maxChain = depth;
            }
            if (popped >= 5) {
                G.Particles.showAction(popped + ' POP!');
                G.Audio.big(popped);
            }
            // Apply pop
            var res = popChains(board, groups);
            blocksPopped += res.removed;

            // Light shake on big pops
            if (popped >= 4) G.Particles.shake(160, 3 + Math.min(5, popped / 2));

            // Sprint / puzzle completion checks
            if (mode === 'sprint' && blocksPopped >= sprintTarget) {
                finished = true;
                break;
            }
            if (mode === 'puzzle' && isBoardCleared()) {
                finished = true;
                break;
            }
        }
        // Level up
        var wantLevel = 1 + Math.floor(score / 5000);
        if (wantLevel > level) {
            level = wantLevel;
            G.Audio.levelUp();
            G.Particles.showAction('LEVEL ' + level);
        }
        chainDepth = 0;
        if (mode === 'puzzle' && puzzleMovesLeft <= 0 && !finished) {
            // Out of moves, puzzle failed.
            gameOver = true;
        }
        updateHUD();
    }

    function isBoardCleared() {
        for (var c = 0; c < COLS; c++) if (board[c].length) return false;
        return true;
    }

    // ---- Rising / tick ------------------------------------------------------

    function tick(dt) {
        if (gameOver || finished) return;
        gameTime += dt;

        // Top-out check runs every tick (columns at ROWS = game over).
        if (isToppedOut()) {
            gameOver = true;
            G.Audio.gameover();
            return;
        }

        // Emergency brake
        if (brakeActive > 0) brakeActive -= dt;
        if (brakeCooldown > 0) brakeCooldown -= dt;

        // Rising speed: base px/sec * level acceleration * user rise multiplier.
        if (mode !== 'puzzle') {
            var userMul = (G.Storage.settings.riseSpeed || 10) / 10;
            var levelMul = 1 + (level - 1) * 0.12;
            var speed = riseSpeedPx * levelMul * userMul;
            if (brakeActive > 0) speed *= 0.2;
            riseProgress += (dt / 1000) * speed;
            if (riseProgress >= layout.cell) {
                riseProgress -= layout.cell;
                spawnRow(board, gameRng);
                // Post-spawn: maybe resolve chains (unlikely since bottom)
                resolveChains();
                // Check top-out.
                if (isToppedOut()) {
                    gameOver = true;
                    G.Audio.gameover();
                }
                // Low-buzz warn when nearly full
                var nearTop = false;
                for (var c = 0; c < COLS; c++) {
                    if (board[c].length >= ROWS - 2) { nearTop = true; break; }
                }
                if (nearTop) G.Audio.warn();
            }
            // Compute time until next rise for HUD.
            var pxPerSec = speed;
            if (pxPerSec > 0) nextRiseAccum = (layout.cell - riseProgress) / pxPerSec;
        } else {
            nextRiseAccum = puzzleMovesLeft;
        }
        updateHUD();
    }

    function isToppedOut() {
        for (var c = 0; c < COLS; c++) if (board[c].length > ROWS) return true;
        // Being at row ROWS is edge-acceptable; top-out when > ROWS.
        for (var c2 = 0; c2 < COLS; c2++) if (board[c2].length >= ROWS + 1) return true;
        // Also: if holding a block and column already at ROWS, next place will top.
        // Strict: if any column already at ROWS, we consider it topped out.
        for (var c3 = 0; c3 < COLS; c3++) if (board[c3].length >= ROWS) return true;
        return false;
    }

    // ---- HUD ----------------------------------------------------------------

    function updateHUD() {
        Hud.text('#hud-score', String(score));
        Hud.text('#hud-level', String(level));
        var ex = document.getElementById('hud-extra');
        var lb = document.getElementById('hud-extra-label');
        if (mode === 'sprint') {
            if (lb) lb.textContent = 'LEFT';
            if (ex) ex.textContent = String(Math.max(0, sprintTarget - blocksPopped));
        } else if (mode === 'puzzle') {
            if (lb) lb.textContent = 'MOVES';
            if (ex) ex.textContent = String(puzzleMovesLeft);
        } else {
            if (lb) lb.textContent = 'NEXT RISE';
            if (ex) ex.textContent = (nextRiseAccum != null ? nextRiseAccum.toFixed(1) : '0') + 's';
        }
        var cl = document.getElementById('hud-combo-label');
        var cv = document.getElementById('hud-combo');
        if (maxChain >= 2) {
            if (cl) cl.style.display = 'block';
            if (cv) { cv.style.display = 'block'; cv.textContent = 'x' + maxChain; }
        } else {
            if (cl) cl.style.display = 'none';
            if (cv) cv.style.display = 'none';
        }
    }

    // ---- Drawing ------------------------------------------------------------

    function drawBackground(ctx, W, H, t) {
        // Subtle animated vertical gradient.
        var phase = (t || 0) * 0.00015;
        var shiftA = 0.05 + 0.03 * Math.sin(phase);
        ctx.fillStyle = '#050810';
        ctx.fillRect(0, 0, W, H);
        // A horizon glow
        ctx.globalAlpha = 0.25 + shiftA;
        ctx.fillStyle = '#0a2238';
        ctx.fillRect(0, H * 0.55, W, H * 0.45);
        ctx.globalAlpha = 1.0;

        // Starfield (slow drift)
        var stars = 42;
        for (var i = 0; i < stars; i++) {
            var sx = ((i * 83) % W + ((t || 0) * 0.015 + i * 7) % 40) % W;
            var sy = ((i * 137) % H + ((t || 0) * 0.008) % 40) % H;
            ctx.globalAlpha = 0.15 + ((i % 5) * 0.04);
            ctx.fillStyle = '#99c6ff';
            ctx.fillRect(sx, sy, 2, 2);
        }
        ctx.globalAlpha = 1.0;
    }

    function drawBlockShape(ctx, cx, cy, size, color, special, colorblind) {
        var pad = 2;
        var s = size - pad * 2;
        var x = cx - s / 2, y = cy - s / 2;

        // Gradient-ish rounded-rect
        ctx.fillStyle = color;
        ctx.fillRect(x, y, s, s);
        // Highlight
        ctx.fillStyle = 'rgba(255,255,255,0.22)';
        ctx.fillRect(x, y, s, Math.max(2, Math.floor(s * 0.15)));
        ctx.fillRect(x, y, Math.max(2, Math.floor(s * 0.15)), s);
        // Shadow
        ctx.fillStyle = 'rgba(0,0,0,0.28)';
        ctx.fillRect(x, y + s - Math.max(2, Math.floor(s * 0.15)), s, Math.max(2, Math.floor(s * 0.15)));
        ctx.fillRect(x + s - Math.max(2, Math.floor(s * 0.15)), y, Math.max(2, Math.floor(s * 0.15)), s);

        // Colorblind glyph
        if (colorblind) {
            ctx.fillStyle = 'rgba(0,0,0,0.55)';
            var gs = Math.floor(s * 0.38);
            var gx = cx, gy = cy;
            // Very simple emblem per color index: size indicates shape.
            ctx.fillRect(gx - 1, gy - gs / 2, 2, gs);
            ctx.fillRect(gx - gs / 2, gy - 1, gs, 2);
        }

        // Special overlay
        if (special === SPECIAL_STAR) {
            ctx.fillStyle = '#fff4a8';
            // small diamond
            var ss = Math.floor(s * 0.32);
            ctx.fillRect(cx - 1, cy - ss, 2, ss * 2);
            ctx.fillRect(cx - ss, cy - 1, ss * 2, 2);
            ctx.fillRect(cx - ss/2, cy - ss/2, ss, 2);
            ctx.fillRect(cx - ss/2, cy + ss/2 - 2, ss, 2);
        } else if (special === SPECIAL_BOMB) {
            ctx.fillStyle = '#222';
            var br = Math.floor(s * 0.28);
            ctx.fillRect(cx - br, cy - br, br * 2, br * 2);
            ctx.fillStyle = '#ff6f3b';
            ctx.fillRect(cx - 2, cy - br - 3, 4, 3);
        } else if (special === SPECIAL_RAINBOW) {
            // Rainbow stripes
            var bandH = Math.max(2, Math.floor(s / 6));
            var bandColors = ['#ff4d6d', '#ffb74d', '#ffeb3b', '#66e676', '#4ad6ff', '#c47bff'];
            for (var bi = 0; bi < bandColors.length; bi++) {
                ctx.fillStyle = bandColors[bi];
                ctx.fillRect(x, y + bi * bandH, s, bandH);
            }
        }
    }

    function drawBoard(ctx, W, H, t) {
        calcLayout(W, H);
        drawBackground(ctx, W, H, t);

        // Frame
        ctx.strokeStyle = '#1f3450';
        ctx.lineWidth = 2;
        ctx.strokeRect(layout.ox - 1, layout.oy - 1, layout.w + 2, layout.h + 2);

        // Grid
        ctx.strokeStyle = '#10172a';
        for (var c = 0; c <= COLS; c++) {
            var xx = layout.ox + c * layout.cell;
            ctx.strokeRect(xx, layout.oy, 0, layout.h);
        }
        for (var rr = 0; rr <= ROWS; rr++) {
            var yy = layout.oy + rr * layout.cell;
            ctx.strokeRect(layout.ox, yy, layout.w, 0);
        }

        // Danger line (top 2 rows)
        ctx.globalAlpha = 0.12;
        ctx.fillStyle = '#ff4d6d';
        ctx.fillRect(layout.ox, layout.oy, layout.w, layout.cell * 2);
        ctx.globalAlpha = 1.0;

        // Blocks. Each block r (bottom-first) draws at
        // y = oy + h - (r+1)*cell  shifted up by riseProgress.
        var colorblind = !!G.Storage.settings.colorBlind;
        for (var cc = 0; cc < COLS; cc++) {
            var col = board[cc];
            for (var r = 0; r < col.length; r++) {
                var b = col[r];
                if (!b) continue;
                var cx = layout.ox + cc * layout.cell + layout.cell / 2;
                var cy = layout.oy + layout.h - (r + 0.5) * layout.cell - riseProgress;
                if (cy + layout.cell / 2 < layout.oy) continue;
                drawBlockShape(ctx, cx, cy, layout.cell, COLORS[b.color], b.special, colorblind);
            }
        }

        // Flash overlays (pop animation)
        G.Particles.drawFlashes(ctx);

        // Carrier drawing
        drawCarrier(ctx);

        // Brake cooldown bar above board
        if (brakeCooldown > 0 || brakeActive > 0) {
            var bw = layout.w;
            var bx = layout.ox;
            var by = layout.oy - 14;
            ctx.fillStyle = '#223045';
            ctx.fillRect(bx, by, bw, 6);
            if (brakeActive > 0) {
                ctx.fillStyle = '#8ae0ff';
                ctx.fillRect(bx, by, bw * (brakeActive / 3000), 6);
            } else {
                ctx.fillStyle = '#ffd873';
                ctx.fillRect(bx, by, bw * (1 - brakeCooldown / 10000), 6);
            }
        }

        // Particles over everything
        G.Particles.drawParticles(ctx);
    }

    function drawCarrier(ctx) {
        var cx = layout.ox + carrier.col * layout.cell + layout.cell / 2;
        var top = layout.oy - 24;
        // Triangle/tractor body
        ctx.fillStyle = '#4da8ff';
        var w = layout.cell * 0.85, h = 18;
        ctx.fillRect(cx - w / 2, top - h, w, h);
        ctx.fillStyle = '#dbefff';
        ctx.fillRect(cx - w / 2 + 3, top - h + 3, w - 6, 4);
        // Tractor beam indicator
        ctx.globalAlpha = 0.18;
        ctx.fillStyle = carrier.heldStack.length ? '#ffd873' : '#8ae0ff';
        ctx.fillRect(cx - w / 3, top, (w / 3) * 2, layout.oy - top);
        ctx.globalAlpha = 1.0;

        // Held stack preview to the right of the carrier
        for (var i = 0; i < carrier.heldStack.length; i++) {
            var hb = carrier.heldStack[i];
            var hx = cx + (w / 2) + 8 + i * (layout.cell * 0.5);
            var hy = top - h / 2;
            drawBlockShape(ctx, hx, hy, layout.cell * 0.5, COLORS[hb.color], hb.special,
                !!G.Storage.settings.colorBlind);
        }
    }

    // ---- Mouse support ------------------------------------------------------

    function columnAt(x) {
        if (x < layout.ox || x >= layout.ox + layout.w) return -1;
        return Math.floor((x - layout.ox) / layout.cell);
    }

    function mouseClick(x, y) {
        var col = columnAt(x);
        if (col < 0) return;
        moveTo(col);
        interact();
    }

    function mouseWheel(dy) { shuffleHeld(); }

    // ---- Getters / test hooks ----------------------------------------------

    function getBoard()   { return board; }
    function getScore()   { return score; }
    function getLevel()   { return level; }
    function getLayout()  { return layout; }
    function getCarrier() { return carrier; }
    function isGameOver() { return gameOver; }
    function isFinished() { return finished; }
    function getMode()    { return mode; }
    function getStats() {
        return {
            score: score, level: level, blocksPopped: blocksPopped,
            maxChain: maxChain, bestChain: bestChain, gameTime: gameTime,
            mode: mode, finished: finished
        };
    }

    function setBoard(b) { board = b; }
    function setScore(v) { score = v; }
    function setGameOver(v) { gameOver = !!v; }

    // Expose constants
    return {
        // constants
        COLS: COLS, ROWS: ROWS, NUM_COLORS: NUM_COLORS,
        SPECIAL_NONE: SPECIAL_NONE, SPECIAL_STAR: SPECIAL_STAR,
        SPECIAL_BOMB: SPECIAL_BOMB, SPECIAL_RAINBOW: SPECIAL_RAINBOW,
        COLORS: COLORS, SHAPES: SHAPES,
        HOLD_MAX: HOLD_MAX,

        // pure helpers (tested)
        makeEmptyBoard: makeEmptyBoard,
        cloneBoard: cloneBoard,
        findChains: findChains,
        popChains: popChains,
        expandSpecials: expandSpecials,
        settle: settle,
        spawnRow: spawnRow,
        seedBoard: seedBoard,
        makeRng: makeRng,
        setSeed: setSeed,

        // lifecycle
        startGame: startGame,
        tick: tick,
        calcLayout: calcLayout,
        draw: drawBoard,
        updateHUD: updateHUD,

        // input
        moveLeft: moveLeft,
        moveRight: moveRight,
        moveTo: moveTo,
        pick: pick,
        place: place,
        interact: interact,
        shuffleHeld: shuffleHeld,
        emergencyBrake: emergencyBrake,
        mouseClick: mouseClick,
        mouseWheel: mouseWheel,

        // getters
        getBoard: getBoard,
        getScore: getScore,
        getLevel: getLevel,
        getLayout: getLayout,
        getCarrier: getCarrier,
        isGameOver: isGameOver,
        isFinished: isFinished,
        getMode: getMode,
        getStats: getStats,

        // test setters
        setBoard: setBoard,
        setScore: setScore,
        setGameOver: setGameOver,
        resolveChains: resolveChains
    };
})();
