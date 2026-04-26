// board.js — Gemswap board state, match detection, cascades, rendering.
'use strict';
var G = G || {};

G.Board = (function () {
    var ROWS = 8;
    var COLS = 8;
    var COLORS_N = 7;

    // Gem type constants. 1..7 are the normal colors. Specials are bit flags
    // applied on top of a base color (except HYPER which is a standalone neutral).
    var SPECIAL_NONE  = 0;
    var SPECIAL_FLAME = 1;   // match-4: 3x3 burst
    var SPECIAL_STAR  = 2;   // L/T match: row+column
    var SPECIAL_HYPER = 3;   // match-5 line: clear all of swapped color

    // Visual palette. Index 0 unused; 1..7 are gem colors.
    var PALETTE = [
        null,
        { name: 'ruby',     core: '#ff5a7a', rim: '#ffb3c3', dark: '#a01030' },
        { name: 'sapphire', core: '#4fa0ff', rim: '#b0d6ff', dark: '#153a78' },
        { name: 'emerald',  core: '#4fd48a', rim: '#b0f0c9', dark: '#15683f' },
        { name: 'topaz',    core: '#ffc850', rim: '#ffe6a0', dark: '#8c6410' },
        { name: 'amethyst', core: '#c882ff', rim: '#e6c6ff', dark: '#5a2a88' },
        { name: 'citrine',  core: '#ffee7d', rim: '#fff6c0', dark: '#a08a10' },
        { name: 'onyx',     core: '#7a90b0', rim: '#c0cce0', dark: '#2a3448' },
    ];

    // State
    var grid = [];             // [r][c] = { color:1..7, special:0..3, frozen:bool } or null
    var rows = ROWS, cols = COLS;
    var layout = { ox: 0, oy: 0, cell: 64, boardW: 0, boardH: 0 };

    var score = 0;
    var level = 1;
    var mode = 'classic';
    var moves = 0;             // classic: move count; timed: unused; puzzle: moves used
    var modeTimer = 0;         // timed: ms remaining
    var puzzleIndex = 0;
    var frozenRemaining = 0;

    var chain = 0;             // current cascade depth
    var maxChain = 0;
    var gameTime = 0;
    var finished = false;
    var gameOverFlag = false;

    // Selection / animation state.
    var sel = null;            // {r,c} first pick
    var cursor = { r: 4, c: 4, active: false };
    var idleTimer = 0;
    var hint = null;           // {r,c,dr,dc} highlighted valid move
    var hintDelay = 5000;

    // Animations
    var animating = false;
    var anims = [];            // active animation list
    var pendingFalls = null;   // after match clear, enqueue falls
    var flashTiles = [];       // tiles currently flashing before removal
    var flashTimer = 0;
    var swapPair = null;       // {a,b,back} active swap tween
    var swapTime = 0;
    var SWAP_MS = 180;
    var FLASH_MS = 240;          // base; recalculated per cascade from group size
    var FALL_MS = 140;
    var SHATTER_MS = 140;        // per-tile shatter animation length
    var SHATTER_STAGGER_MS = 32; // delay between tiles shattering in a group
    var SETTLE_MS = 40;          // breath between cascades
    var CHARGE_MS = 35;          // brief charge-up flash before each tile shatters

    var flashDuration = FLASH_MS;
    var settleTimer = 0;
    var shakeAmp = 0;
    var shakeT = 0;

    // Stats
    var stats = null;

    // Scoring helpers
    function baseScore(len) {
        if (len >= 5) return 150;
        if (len === 4) return 100;
        return 50;
    }

    // ------------------------------------------------------------------
    // Pure helpers (testable)
    // ------------------------------------------------------------------

    function inBounds(r, c) { return r >= 0 && r < rows && c >= 0 && c < cols; }

    function cellColor(g) { return g ? g.color : 0; }

    // Find all matches in a grid. Returns { groups:[ [r,c,...] ], shape:{ lens, starCenter } }.
    function findMatches(g) {
        var matched = {};
        var groups = [];
        var r, c, i, run, color;
        // horizontal runs
        var hRuns = [];
        for (r = 0; r < rows; r++) {
            c = 0;
            while (c < cols) {
                color = cellColor(g[r][c]);
                if (!color) { c++; continue; }
                var start = c;
                while (c < cols && cellColor(g[r][c]) === color) c++;
                run = c - start;
                if (run >= 3) hRuns.push({ r: r, c: start, len: run, horiz: true });
            }
        }
        var vRuns = [];
        for (c = 0; c < cols; c++) {
            r = 0;
            while (r < rows) {
                color = cellColor(g[r][c]);
                if (!color) { r++; continue; }
                var start2 = r;
                while (r < rows && cellColor(g[r][c]) === color) r++;
                run = r - start2;
                if (run >= 3) vRuns.push({ r: start2, c: c, len: run, horiz: false });
            }
        }

        // Merge into groups by connectivity (L/T shapes = one group spanning
        // horiz and vert runs that share a cell).
        function runCells(run) {
            var out = [];
            for (var k = 0; k < run.len; k++) {
                if (run.horiz) out.push([run.r, run.c + k]);
                else out.push([run.r + k, run.c]);
            }
            return out;
        }
        var all = hRuns.concat(vRuns);
        var groupOf = new Array(all.length);
        for (i = 0; i < all.length; i++) groupOf[i] = i;
        function find(i) { while (groupOf[i] !== i) { groupOf[i] = groupOf[groupOf[i]]; i = groupOf[i]; } return i; }
        function union(a, b) { a = find(a); b = find(b); if (a !== b) groupOf[a] = b; }

        // Same-color connection via shared cell.
        for (i = 0; i < all.length; i++) {
            for (var j = i + 1; j < all.length; j++) {
                if (cellColorOfRun(g, all[i]) !== cellColorOfRun(g, all[j])) continue;
                var ci = runCells(all[i]);
                var cj = runCells(all[j]);
                var shared = false;
                for (var a = 0; a < ci.length && !shared; a++) {
                    for (var b = 0; b < cj.length && !shared; b++) {
                        if (ci[a][0] === cj[b][0] && ci[a][1] === cj[b][1]) shared = true;
                    }
                }
                if (shared) union(i, j);
            }
        }

        var buckets = {};
        for (i = 0; i < all.length; i++) {
            var root = find(i);
            if (!buckets[root]) buckets[root] = { runs: [], cells: {} };
            buckets[root].runs.push(all[i]);
            var cells = runCells(all[i]);
            for (var k = 0; k < cells.length; k++) {
                var key = cells[k][0] + ',' + cells[k][1];
                buckets[root].cells[key] = cells[k];
                matched[key] = true;
            }
        }

        for (var rootKey in buckets) {
            if (!Object.prototype.hasOwnProperty.call(buckets, rootKey)) continue;
            var bucket = buckets[rootKey];
            var cellList = [];
            for (var k2 in bucket.cells) cellList.push(bucket.cells[k2]);
            // Classify shape for special gem award.
            var special = SPECIAL_NONE;
            var lensH = 0, lensV = 0;
            var hasH = false, hasV = false;
            for (var rr = 0; rr < bucket.runs.length; rr++) {
                var rn = bucket.runs[rr];
                if (rn.horiz) { hasH = true; lensH = Math.max(lensH, rn.len); }
                else          { hasV = true; lensV = Math.max(lensV, rn.len); }
            }
            var maxLine = Math.max(lensH, lensV);
            if (hasH && hasV) {
                // L/T or cross — treat as starlight. Even if both legs are only 3,
                // crossing counts as a star (classic genre rule variant).
                special = SPECIAL_STAR;
            } else if (maxLine >= 5) {
                special = SPECIAL_HYPER;
            } else if (maxLine === 4) {
                special = SPECIAL_FLAME;
            }
            groups.push({
                cells: cellList,
                color: cellColorOfRun(g, bucket.runs[0]),
                special: special,
                size: cellList.length,
                maxLine: maxLine,
            });
        }
        return groups;
    }

    function cellColorOfRun(g, run) { return cellColor(g[run.r][run.c]); }

    // Would a swap of (r1,c1)<->(r2,c2) in grid produce any match?
    function swapMakesMatch(g, r1, c1, r2, c2) {
        if (!inBounds(r1, c1) || !inBounds(r2, c2)) return false;
        var g1 = g[r1][c1], g2 = g[r2][c2];
        if (!g1 || !g2) return false;
        // Hyper gem swap always "matches" (clears whole color).
        if ((g1 && g1.special === SPECIAL_HYPER) || (g2 && g2.special === SPECIAL_HYPER)) return true;
        g[r1][c1] = g2;
        g[r2][c2] = g1;
        var matches = findMatches(g);
        g[r1][c1] = g1;
        g[r2][c2] = g2;
        return matches.length > 0;
    }

    // Find a valid move anywhere on the board. Returns {r,c,dr,dc} or null.
    function findAnyMove(g) {
        for (var r = 0; r < rows; r++) {
            for (var c = 0; c < cols; c++) {
                if (c + 1 < cols && swapMakesMatch(g, r, c, r, c + 1)) return { r: r, c: c, dr: 0, dc: 1 };
                if (r + 1 < rows && swapMakesMatch(g, r, c, r + 1, c)) return { r: r, c: c, dr: 1, dc: 0 };
            }
        }
        return null;
    }

    function isDeadlocked(g) { return findAnyMove(g) === null; }

    // Create an empty grid (null rows).
    function makeEmptyGrid() {
        var g = [];
        for (var r = 0; r < rows; r++) {
            var row = [];
            for (var c = 0; c < cols; c++) row.push(null);
            g.push(row);
        }
        return g;
    }

    // Seed a grid without any pre-existing matches, with at least one valid move.
    function seedGrid(seedRand) {
        var rand = seedRand || Math.random;
        var g, attempts = 0;
        while (attempts++ < 200) {
            g = makeEmptyGrid();
            for (var r = 0; r < rows; r++) {
                for (var c = 0; c < cols; c++) {
                    var tries = 0;
                    while (tries++ < 40) {
                        var color = 1 + Math.floor(rand() * COLORS_N);
                        // avoid immediate matches as we fill
                        if (c >= 2 && g[r][c-1] && g[r][c-2] &&
                            g[r][c-1].color === color && g[r][c-2].color === color) continue;
                        if (r >= 2 && g[r-1][c] && g[r-2][c] &&
                            g[r-1][c].color === color && g[r-2][c].color === color) continue;
                        g[r][c] = { color: color, special: SPECIAL_NONE, frozen: false };
                        break;
                    }
                    if (!g[r][c]) g[r][c] = { color: 1, special: SPECIAL_NONE, frozen: false };
                }
            }
            if (findMatches(g).length === 0 && findAnyMove(g) !== null) return g;
        }
        return g; // best effort
    }

    function scoreChain(groupsSizes, chainDepth) {
        var total = 0;
        for (var i = 0; i < groupsSizes.length; i++) {
            total += baseScore(groupsSizes[i]);
        }
        var multiplier = chainDepth + 1;
        return total * multiplier;
    }

    // ------------------------------------------------------------------
    // Game state management
    // ------------------------------------------------------------------

    function startGame(selectedMode) {
        mode = selectedMode || 'classic';
        score = 0;
        level = 1;
        moves = 0;
        chain = 0;
        maxChain = 0;
        gameTime = 0;
        finished = false;
        gameOverFlag = false;
        sel = null;
        cursor = { r: 4, c: 4, active: false };
        idleTimer = 0;
        hint = null;
        animating = false;
        anims = [];
        flashTiles = [];
        swapPair = null;
        settleTimer = 0;
        shakeAmp = 0;
        shakeT = 0;
        flashDuration = FLASH_MS;
        stats = { swaps: 0, matches: 0, flameMade: 0, starMade: 0, hyperMade: 0, maxChain: 0 };

        if (mode === 'timed') {
            modeTimer = 120000; // 2 minutes
        } else if (mode === 'puzzle') {
            puzzleIndex = 0;
            loadPuzzle(puzzleIndex);
            return;
        }
        grid = seedGrid();
    }

    function loadPuzzle(idx) {
        if (!G.Puzzles) { grid = seedGrid(); return; }
        var p = G.Puzzles.get(idx);
        grid = makeEmptyGrid();
        frozenRemaining = 0;
        for (var r = 0; r < rows; r++) {
            for (var c = 0; c < cols; c++) {
                var code = p[r] && p[r][c];
                var frozen = false, color = 0;
                if (typeof code === 'string' && code.length > 0) {
                    if (code[0] === 'F') { frozen = true; code = code.substring(1); }
                    color = parseInt(code, 10);
                }
                if (!color) color = 1 + Math.floor(Math.random() * COLORS_N);
                grid[r][c] = { color: color, special: SPECIAL_NONE, frozen: frozen };
                if (frozen) frozenRemaining++;
            }
        }
        // Nudge: if seeded puzzle has immediate matches, reshuffle colors only (keep frozen).
        var guard = 0;
        while (findMatches(grid).length > 0 && guard++ < 50) {
            for (var rr = 0; rr < rows; rr++) {
                for (var cc = 0; cc < cols; cc++) {
                    if (!grid[rr][cc].frozen) {
                        grid[rr][cc].color = 1 + Math.floor(Math.random() * COLORS_N);
                    }
                }
            }
        }
        if (findAnyMove(grid) === null) {
            // fallback — if somehow deadlocked, drop to normal seed keeping frozen.
            shuffleBoard();
        }
    }

    function nextPuzzle() {
        puzzleIndex++;
        if (!G.Puzzles || puzzleIndex >= G.Puzzles.count()) {
            finished = true;
            return false;
        }
        loadPuzzle(puzzleIndex);
        return true;
    }

    function shuffleBoard() {
        var nonFrozen = [];
        for (var r = 0; r < rows; r++) {
            for (var c = 0; c < cols; c++) {
                if (grid[r][c] && !grid[r][c].frozen && grid[r][c].special === SPECIAL_NONE) {
                    nonFrozen.push(grid[r][c]);
                }
            }
        }
        // Fisher-Yates
        var tries = 0;
        while (tries++ < 100) {
            for (var i = nonFrozen.length - 1; i > 0; i--) {
                var j = Math.floor(Math.random() * (i + 1));
                var tmp = nonFrozen[i]; nonFrozen[i] = nonFrozen[j]; nonFrozen[j] = tmp;
            }
            var idx = 0;
            for (var r2 = 0; r2 < rows; r2++) {
                for (var c2 = 0; c2 < cols; c2++) {
                    if (grid[r2][c2] && !grid[r2][c2].frozen && grid[r2][c2].special === SPECIAL_NONE) {
                        grid[r2][c2] = nonFrozen[idx++];
                    }
                }
            }
            if (findMatches(grid).length === 0 && findAnyMove(grid) !== null) return;
        }
    }

    // ------------------------------------------------------------------
    // Layout and drawing
    // ------------------------------------------------------------------

    function calcLayout(W, H) {
        var maxBoard = Math.min(W - 200, H - 80);
        var cell = Math.floor(maxBoard / cols);
        if (cell < 32) cell = 32;
        if (cell > 72) cell = 72;
        layout.cell = cell;
        layout.boardW = cell * cols;
        layout.boardH = cell * rows;
        layout.ox = Math.floor((W - layout.boardW) / 2 - 60);
        if (layout.ox < 30) layout.ox = 30;
        layout.oy = Math.floor((H - layout.boardH) / 2);
        if (layout.oy < 40) layout.oy = 40;
    }

    function cellXY(r, c) {
        return { x: layout.ox + c * layout.cell, y: layout.oy + r * layout.cell };
    }

    function pointToCell(px, py) {
        var dx = px - layout.ox, dy = py - layout.oy;
        if (dx < 0 || dy < 0) return null;
        var c = Math.floor(dx / layout.cell);
        var r = Math.floor(dy / layout.cell);
        if (!inBounds(r, c)) return null;
        return { r: r, c: c };
    }

    function drawBackground(ctx, W, H) {
        ctx.fillStyle = '#0a0612';
        ctx.fillRect(0, 0, W, H);
        // starfield-ish
        var t = (gameTime * 0.0001) % 1;
        ctx.save();
        for (var i = 0; i < 40; i++) {
            var x = ((i * 97.31 + t * W) % W);
            var y = ((i * 173.7) % H);
            var s = 0.3 + (i % 5) * 0.15;
            ctx.fillStyle = 'rgba(180,120,220,' + (0.06 + (i % 3) * 0.04) + ')';
            ctx.fillRect(x, y, s, s);
        }
        ctx.restore();
    }

    function drawBoardFrame(ctx) {
        var x = layout.ox - 8, y = layout.oy - 8;
        var w = layout.boardW + 16, h = layout.boardH + 16;
        ctx.fillStyle = 'rgba(30, 15, 50, 0.7)';
        ctx.fillRect(x, y, w, h);
        ctx.strokeStyle = '#3a2a55';
        ctx.lineWidth = 2;
        ctx.strokeRect(x + 0.5, y + 0.5, w - 1, h - 1);
        // cells
        for (var r = 0; r < rows; r++) {
            for (var c = 0; c < cols; c++) {
                var p = cellXY(r, c);
                ctx.fillStyle = ((r + c) & 1) ? 'rgba(40,25,65,0.55)' : 'rgba(30,18,50,0.55)';
                ctx.fillRect(p.x, p.y, layout.cell, layout.cell);
                if (grid[r][c] && grid[r][c].frozen) {
                    ctx.fillStyle = 'rgba(140,200,255,0.15)';
                    ctx.fillRect(p.x + 2, p.y + 2, layout.cell - 4, layout.cell - 4);
                    ctx.strokeStyle = 'rgba(180,220,255,0.4)';
                    ctx.lineWidth = 1;
                    ctx.strokeRect(p.x + 2.5, p.y + 2.5, layout.cell - 5, layout.cell - 5);
                }
            }
        }
    }

    // Draw a gem shape based on color index. Different colors get different silhouettes.
    function drawGem(ctx, g, cx, cy, size, opts) {
        opts = opts || {};
        if (!g) return;
        var pal = PALETTE[g.color];
        if (!pal) return;
        var s = size * 0.78;
        var half = s / 2;
        var pulse = opts.pulse || 0;
        s *= (1 + pulse * 0.08);
        half = s / 2;

        ctx.save();
        ctx.translate(cx, cy);

        var shape = g.color % 7;
        ctx.beginPath();
        if (g.special === SPECIAL_HYPER) {
            // starburst
            var N = 8;
            for (var i = 0; i < N * 2; i++) {
                var ang = (i / (N * 2)) * Math.PI * 2 - Math.PI / 2;
                var rr = (i & 1) ? half * 0.45 : half;
                var xx = Math.cos(ang) * rr, yy = Math.sin(ang) * rr;
                if (i === 0) ctx.moveTo(xx, yy); else ctx.lineTo(xx, yy);
            }
            ctx.closePath();
        } else if (shape === 1) {
            // ruby — diamond
            ctx.moveTo(0, -half); ctx.lineTo(half, 0); ctx.lineTo(0, half); ctx.lineTo(-half, 0); ctx.closePath();
        } else if (shape === 2) {
            // sapphire — hexagon
            polygon(ctx, 6, half, Math.PI / 6);
        } else if (shape === 3) {
            // emerald — rectangle w/ cut corners (octagon flat)
            polygon(ctx, 8, half, Math.PI / 8);
        } else if (shape === 4) {
            // topaz — 5-point star
            star(ctx, 5, half, half * 0.5, -Math.PI / 2);
        } else if (shape === 5) {
            // amethyst — triangle (pointing up)
            polygon(ctx, 3, half, -Math.PI / 2);
        } else if (shape === 6) {
            // citrine — circle
            ctx.arc(0, 0, half, 0, Math.PI * 2);
        } else {
            // onyx (shape 0 => color 7) — pentagon
            polygon(ctx, 5, half, -Math.PI / 2);
        }

        var grad = ctx.createRadialGradient(-half * 0.3, -half * 0.3, 1, 0, 0, half);
        grad.addColorStop(0, pal.rim);
        grad.addColorStop(0.55, pal.core);
        grad.addColorStop(1, pal.dark);
        ctx.fillStyle = grad;
        ctx.fill();

        ctx.lineWidth = 1.4;
        ctx.strokeStyle = pal.dark;
        ctx.stroke();

        // highlight
        ctx.beginPath();
        ctx.ellipse(-half * 0.32, -half * 0.42, half * 0.28, half * 0.14, -0.5, 0, Math.PI * 2);
        ctx.fillStyle = 'rgba(255,255,255,0.55)';
        ctx.fill();

        // specials: glyph overlay
        if (g.special === SPECIAL_FLAME) {
            ctx.fillStyle = 'rgba(255,230,120,0.9)';
            ctx.beginPath();
            ctx.arc(0, 0, half * 0.35, 0, Math.PI * 2);
            ctx.fill();
        } else if (g.special === SPECIAL_STAR) {
            ctx.strokeStyle = 'rgba(255,255,255,0.85)';
            ctx.lineWidth = 2;
            ctx.beginPath();
            ctx.moveTo(-half * 0.8, 0); ctx.lineTo(half * 0.8, 0);
            ctx.moveTo(0, -half * 0.8); ctx.lineTo(0, half * 0.8);
            ctx.stroke();
        }

        ctx.restore();
    }

    function polygon(ctx, n, r, rot) {
        for (var i = 0; i < n; i++) {
            var a = rot + (i / n) * Math.PI * 2;
            var x = Math.cos(a) * r, y = Math.sin(a) * r;
            if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
        }
        ctx.closePath();
    }
    function star(ctx, spikes, outer, inner, rot) {
        for (var i = 0; i < spikes * 2; i++) {
            var a = rot + (i / (spikes * 2)) * Math.PI * 2;
            var r = (i & 1) ? inner : outer;
            var x = Math.cos(a) * r, y = Math.sin(a) * r;
            if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
        }
        ctx.closePath();
    }

    function drawSelection(ctx) {
        if (!sel) return;
        var p = cellXY(sel.r, sel.c);
        var pulse = 0.5 + 0.5 * Math.sin(gameTime * 0.008);
        ctx.save();
        ctx.strokeStyle = 'rgba(255,240,120,' + (0.5 + pulse * 0.5) + ')';
        ctx.lineWidth = 3;
        ctx.strokeRect(p.x + 2.5, p.y + 2.5, layout.cell - 5, layout.cell - 5);
        ctx.restore();
    }

    function drawCursor(ctx) {
        if (!cursor.active) return;
        var p = cellXY(cursor.r, cursor.c);
        ctx.save();
        ctx.strokeStyle = '#c78aff';
        ctx.lineWidth = 2;
        ctx.setLineDash([4, 3]);
        ctx.strokeRect(p.x + 1.5, p.y + 1.5, layout.cell - 3, layout.cell - 3);
        ctx.restore();
    }

    function drawHint(ctx) {
        if (!hint) return;
        var p = cellXY(hint.r, hint.c);
        var pulse = 0.5 + 0.5 * Math.sin(gameTime * 0.01);
        ctx.save();
        ctx.fillStyle = 'rgba(255,240,120,' + (0.12 + pulse * 0.18) + ')';
        ctx.fillRect(p.x, p.y, layout.cell, layout.cell);
        var p2 = cellXY(hint.r + hint.dr, hint.c + hint.dc);
        ctx.fillRect(p2.x, p2.y, layout.cell, layout.cell);
        ctx.restore();
    }

    // Per-tile shatter overlay — radiant glint ring expanding outward as
    // each tile shatters, synced to that tile's individual stagger.
    function drawShatterRings(ctx) {
        if (flashTiles.length === 0) return;
        var cell = layout.cell;
        ctx.save();
        for (var i = 0; i < flashTiles.length; i++) {
            var t = flashTiles[i];
            var local = (flashTimer - t.delay - CHARGE_MS) / SHATTER_MS;
            if (local <= 0 || local >= 1) continue;
            var p = cellXY(t.r, t.c);
            var cx = p.x + cell / 2, cy = p.y + cell / 2;
            var fade = 1 - local;
            // Bright glint ring.
            ctx.strokeStyle = 'rgba(255, 250, 220, ' + (fade * 0.85) + ')';
            ctx.lineWidth = 2.4;
            ctx.beginPath();
            ctx.arc(cx, cy, cell * 0.30 + local * cell * 0.55, 0, Math.PI * 2);
            ctx.stroke();
            // Inner cross-flash for the first half.
            if (local < 0.5) {
                var cf = 1 - local * 2;
                ctx.strokeStyle = 'rgba(255, 255, 255, ' + (cf * 0.7) + ')';
                ctx.lineWidth = 1.6;
                ctx.beginPath();
                ctx.moveTo(cx - cell * 0.45 * cf, cy);
                ctx.lineTo(cx + cell * 0.45 * cf, cy);
                ctx.moveTo(cx, cy - cell * 0.45 * cf);
                ctx.lineTo(cx, cy + cell * 0.45 * cf);
                ctx.stroke();
            }
        }
        ctx.restore();
    }

    function drawBoard(ctx) {
        var sx = 0, sy = 0;
        if (shakeAmp > 0) {
            sx = (Math.random() - 0.5) * shakeAmp;
            sy = (Math.random() - 0.5) * shakeAmp;
        }
        ctx.save();
        ctx.translate(sx, sy);

        drawBoardFrame(ctx);
        drawHint(ctx);
        var cell = layout.cell;

        // Per-tile shatter lookup so the gem-draw loop can render each
        // shattering gem with its own charge flash + scale-out + fade.
        var shatterMap = null;
        if (flashTiles.length > 0) {
            shatterMap = {};
            for (var fpi = 0; fpi < flashTiles.length; fpi++) {
                var fpt = flashTiles[fpi];
                shatterMap[fpt.r + ',' + fpt.c] = fpt;
            }
        }

        // Draw all gems, with anim offsets where present.
        for (var r = 0; r < rows; r++) {
            for (var c = 0; c < cols; c++) {
                var g = grid[r][c];
                if (!g) continue;
                var p = cellXY(r, c);
                var cx = p.x + cell / 2;
                var cy = p.y + cell / 2;
                var pulse = 0;
                var shatterAlpha = 1;
                var shatterScale = 1;
                var shatterRot = 0;
                var shatterCharge = 0;
                var shatterSkip = false;
                if (shatterMap) {
                    var sh = shatterMap[r + ',' + c];
                    if (sh) {
                        var local = flashTimer - sh.delay;
                        if (local < 0) {
                            // not yet
                        } else if (local < CHARGE_MS) {
                            // Charge phase — bright flash, gem slightly bulges.
                            shatterCharge = local / CHARGE_MS;
                            shatterScale = 1 + 0.10 * shatterCharge;
                        } else if (local < CHARGE_MS + SHATTER_MS) {
                            var lt = (local - CHARGE_MS) / SHATTER_MS;
                            shatterAlpha = 1 - lt;
                            shatterScale = 1.10 + lt * 0.7;
                            shatterRot = lt * 0.8;
                        } else {
                            shatterSkip = true;
                        }
                    }
                }
                if (shatterSkip) continue;

                // swap animation
                if (swapPair) {
                    var t = Math.min(1, swapTime / SWAP_MS);
                    var ease = 1 - Math.pow(1 - t, 3);
                    if (swapPair.back) ease = Math.sin(ease * Math.PI); // there and back
                    if (swapPair.a.r === r && swapPair.a.c === c) {
                        var tgt = cellXY(swapPair.b.r, swapPair.b.c);
                        cx = p.x + cell / 2 + (tgt.x - p.x) * ease;
                        cy = p.y + cell / 2 + (tgt.y - p.y) * ease;
                    } else if (swapPair.b.r === r && swapPair.b.c === c) {
                        var tgt2 = cellXY(swapPair.a.r, swapPair.a.c);
                        cx = p.x + cell / 2 + (tgt2.x - p.x) * ease;
                        cy = p.y + cell / 2 + (tgt2.y - p.y) * ease;
                    }
                }
                // fall animation
                for (var i = 0; i < anims.length; i++) {
                    var an = anims[i];
                    if (an.type === 'fall' && an.r === r && an.c === c) {
                        var ft = Math.min(1, an.t / an.dur);
                        var ease2 = 1 - Math.pow(1 - ft, 3);
                        cy = p.y + cell / 2 + (an.fromY - p.y) * (1 - ease2);
                    }
                }
                if (sel && sel.r === r && sel.c === c) pulse = 0.2 * (0.5 + 0.5 * Math.sin(gameTime * 0.015));

                if (shatterAlpha < 1 || shatterScale !== 1 || shatterRot !== 0 || shatterCharge > 0) {
                    ctx.save();
                    ctx.globalAlpha = Math.max(0, shatterAlpha);
                    ctx.translate(cx, cy);
                    ctx.rotate(shatterRot);
                    ctx.scale(shatterScale, shatterScale);
                    drawGem(ctx, g, 0, 0, cell, { pulse: pulse });
                    if (shatterCharge > 0) {
                        // Bright charge-up overlay — quick white flash before shatter.
                        ctx.globalAlpha = 0.55 * shatterCharge;
                        ctx.fillStyle = '#ffffff';
                        ctx.beginPath();
                        ctx.arc(0, 0, cell * 0.36, 0, Math.PI * 2);
                        ctx.fill();
                    }
                    ctx.restore();
                } else {
                    drawGem(ctx, g, cx, cy, cell, { pulse: pulse });
                }
            }
        }
        drawSelection(ctx);
        drawCursor(ctx);
        drawShatterRings(ctx);
        ctx.restore();
    }

    // ------------------------------------------------------------------
    // Input handling
    // ------------------------------------------------------------------

    function isSwapAdjacent(a, b) {
        return (Math.abs(a.r - b.r) + Math.abs(a.c - b.c)) === 1;
    }

    function tryPick(r, c) {
        if (animating) return;
        if (!inBounds(r, c)) return;
        if (!grid[r][c] || grid[r][c].frozen) return;
        if (!sel) {
            sel = { r: r, c: c };
            if (G.AppAudio) G.AppAudio.pick();
            return;
        }
        if (sel.r === r && sel.c === c) {
            sel = null; return;
        }
        if (isSwapAdjacent(sel, { r: r, c: c })) {
            beginSwap(sel.r, sel.c, r, c);
            sel = null;
        } else {
            sel = { r: r, c: c };
            if (G.AppAudio) G.AppAudio.pick();
        }
    }

    function cursorMove(dr, dc) {
        cursor.active = true;
        var nr = Math.max(0, Math.min(rows - 1, cursor.r + dr));
        var nc = Math.max(0, Math.min(cols - 1, cursor.c + dc));
        if (sel && sel.r === cursor.r && sel.c === cursor.c) {
            // swap by arrow
            if (isSwapAdjacent(sel, { r: nr, c: nc })) {
                beginSwap(sel.r, sel.c, nr, nc);
                sel = null;
                cursor.r = nr; cursor.c = nc;
                return;
            }
        }
        cursor.r = nr; cursor.c = nc;
        if (G.AppAudio) G.AppAudio.cursor();
    }

    function cursorConfirm() {
        tryPick(cursor.r, cursor.c);
    }

    function beginSwap(r1, c1, r2, c2) {
        if (!inBounds(r1, c1) || !inBounds(r2, c2)) return;
        var g1 = grid[r1][c1], g2 = grid[r2][c2];
        if (!g1 || !g2 || g1.frozen || g2.frozen) return;
        // visual swap
        swapPair = { a: { r: r1, c: c1 }, b: { r: r2, c: c2 }, back: false };
        swapTime = 0;
        animating = true;
        idleTimer = 0;
        hint = null;
        // plan outcome:
        // Hyper swap — clear all of (non-hyper) color.
        if (g1.special === SPECIAL_HYPER || g2.special === SPECIAL_HYPER) {
            swapPair._hyper = true;
            return;
        }
        // Try swap — does it produce a match?
        grid[r1][c1] = g2;
        grid[r2][c2] = g1;
        var matches = findMatches(grid);
        if (matches.length === 0) {
            // undo; animation will play back
            grid[r1][c1] = g1;
            grid[r2][c2] = g2;
            swapPair.back = true;
        } else {
            swapPair._commit = true;
        }
    }

    function handleClick(px, py) {
        var cell = pointToCell(px, py);
        if (!cell) return;
        tryPick(cell.r, cell.c);
    }

    // ------------------------------------------------------------------
    // Update: animation driver + cascades
    // ------------------------------------------------------------------

    function update(dt) {
        gameTime += dt;
        if (shakeAmp > 0) {
            shakeT -= dt;
            if (shakeT <= 0) { shakeAmp = 0; shakeT = 0; }
        }
        if (mode === 'timed' && !gameOverFlag && !finished) {
            modeTimer -= dt;
            if (modeTimer <= 0) {
                modeTimer = 0;
                finished = true;
                gameOverFlag = true;
            }
        }

        if (!animating) {
            idleTimer += dt;
            if (idleTimer >= hintDelay && !hint) {
                hint = findAnyMove(grid);
                if (!hint) {
                    // deadlock detected: shuffle
                    if (G.AppAudio) G.AppAudio.shuffle();
                    shuffleBoard();
                }
            }
        }

        // Swap animation
        if (swapPair) {
            swapTime += dt;
            if (swapTime >= SWAP_MS) {
                var a = swapPair.a, b = swapPair.b;
                if (swapPair._commit) {
                    // grid already swapped during beginSwap
                    if (G.AppAudio) G.AppAudio.swap(true);
                    stats.swaps++;
                    moves++;
                    chain = 0;
                    swapPair = null;
                    resolveMatches();
                } else if (swapPair._hyper) {
                    // determine colors
                    var h = grid[a.r][a.c].special === SPECIAL_HYPER ? grid[a.r][a.c] : grid[b.r][b.c];
                    var other = (h === grid[a.r][a.c]) ? grid[b.r][b.c] : grid[a.r][a.c];
                    var targetColor = other ? other.color : (1 + Math.floor(Math.random() * COLORS_N));
                    // clear hypergem cell + all matching color
                    var toClear = [];
                    for (var r = 0; r < rows; r++) {
                        for (var c = 0; c < cols; c++) {
                            var gg = grid[r][c];
                            if (!gg) continue;
                            if (gg === h || gg.color === targetColor) toClear.push({ r: r, c: c });
                        }
                    }
                    if (G.AppAudio) G.AppAudio.hyper();
                    clearTiles(toClear, targetColor);
                    stats.swaps++;
                    moves++;
                    chain = 0;
                    swapPair = null;
                    scheduleCollapse();
                } else if (swapPair.back) {
                    if (G.AppAudio) G.AppAudio.swap(false);
                    swapPair = null;
                    animating = false;
                } else {
                    swapPair = null;
                    animating = false;
                }
            }
        }

        // Flash countdown — fire shard bursts as each tile begins shattering.
        if (flashTiles.length > 0) {
            flashTimer += dt;
            if (G.Particles) {
                for (var fi = 0; fi < flashTiles.length; fi++) {
                    var ft = flashTiles[fi];
                    if (ft.burstFired) continue;
                    if (flashTimer < ft.delay + CHARGE_MS) continue;
                    var gb = grid[ft.r][ft.c];
                    var bcol = (gb && PALETTE[gb.color]) ? PALETTE[gb.color].core : '#ffffff';
                    var bp = cellXY(ft.r, ft.c);
                    G.Particles.burst(bp.x + layout.cell / 2, bp.y + layout.cell / 2,
                                      bcol, 8 + chain * 2);
                    ft.burstFired = true;
                }
            }
            if (flashTimer >= flashDuration) {
                // actually remove the tiles
                for (var i = 0; i < flashTiles.length; i++) {
                    var t = flashTiles[i];
                    var gg2 = grid[t.r][t.c];
                    if (gg2 && gg2.frozen) {
                        // frozen tile takes 1 hit to break
                        gg2.frozen = false;
                        frozenRemaining--;
                        continue;
                    }
                    grid[t.r][t.c] = null;
                }
                flashTiles = [];
                flashTimer = 0;
                // upgrade specials (placed)
                if (pendingFalls && pendingFalls.upgrade) {
                    var up = pendingFalls.upgrade;
                    if (inBounds(up.r, up.c) && grid[up.r][up.c] === null) {
                        grid[up.r][up.c] = { color: up.color || (1 + Math.floor(Math.random() * COLORS_N)),
                                             special: up.special, frozen: false };
                        if (up.special === SPECIAL_HYPER) stats.hyperMade++;
                        else if (up.special === SPECIAL_STAR) stats.starMade++;
                        else if (up.special === SPECIAL_FLAME) stats.flameMade++;
                    }
                }
                pendingFalls = { collapse: true };
            }
        }

        // Fall animations
        var done = [];
        for (var k = 0; k < anims.length; k++) {
            var an2 = anims[k];
            if (an2.type === 'fall') {
                an2.t += dt;
                if (an2.t >= an2.dur) done.push(k);
            }
        }
        if (done.length > 0) {
            for (var d = done.length - 1; d >= 0; d--) anims.splice(done[d], 1);
        }

        if (pendingFalls && pendingFalls.collapse && flashTiles.length === 0) {
            pendingFalls = null;
            collapseAndFill();
        }

        if (anims.length === 0 && flashTiles.length === 0 && !swapPair && pendingFalls === null) {
            if (animating) {
                // Brief settle pause between cascades so each chain step
                // reads as its own beat.
                if (chain > 0 && settleTimer < SETTLE_MS) {
                    settleTimer += dt;
                    return;
                }
                // post-settle check
                var m = findMatches(grid);
                if (m.length > 0) {
                    settleTimer = 0;
                    resolveMatchGroups(m);
                } else {
                    settleTimer = 0;
                    animating = false;
                    chain = 0;
                    // end-of-move checks
                    if (mode === 'puzzle' && frozenRemaining === 0) {
                        if (!nextPuzzle()) { /* finished all puzzles */ }
                    }
                    // level up
                    if (mode === 'classic') {
                        var threshold = level * 1000;
                        if (score >= threshold) {
                            level++;
                            if (G.AppAudio) G.AppAudio.levelUp();
                        }
                    }
                    // check deadlock
                    if (findAnyMove(grid) === null) {
                        if (mode === 'classic') {
                            // offer a shuffle; if repeated shuffle fails, game over
                            shuffleBoard();
                            if (findAnyMove(grid) === null) {
                                gameOverFlag = true;
                            }
                        }
                    }
                }
            }
        }
    }

    function resolveMatches() {
        animating = true;
        var m = findMatches(grid);
        if (m.length === 0) { animating = false; return; }
        resolveMatchGroups(m);
    }

    function resolveMatchGroups(groups) {
        chain++;
        if (chain > maxChain) maxChain = chain;
        stats.maxChain = maxChain;
        stats.matches += groups.length;
        // compute score
        var sizes = [];
        for (var i = 0; i < groups.length; i++) sizes.push(groups[i].size);
        var delta = scoreChain(sizes, chain - 1);
        score += delta;

        // sound pitch step on cascade
        if (G.AppAudio) G.AppAudio.match(chain, groups[0].size);

        // Collect all cells to clear; also determine upgrade cells (special gen).
        var cellsToClear = [];
        var upgrade = null;
        for (var g = 0; g < groups.length; g++) {
            var grp = groups[g];
            for (var k = 0; k < grp.cells.length; k++) {
                cellsToClear.push({ r: grp.cells[k][0], c: grp.cells[k][1] });
            }
            if (grp.special !== SPECIAL_NONE && !upgrade) {
                var centerCell = grp.cells[Math.floor(grp.cells.length / 2)];
                upgrade = { r: centerCell[0], c: centerCell[1], special: grp.special, color: grp.color };
                // exclude this cell from clearance (it becomes the special)
                cellsToClear = cellsToClear.filter(function (cc) {
                    return !(cc.r === upgrade.r && cc.c === upgrade.c);
                });
            }
        }

        // Handle special gem detonations if any of the cleared tiles held a special.
        var detonations = [];
        for (var i2 = 0; i2 < cellsToClear.length; i2++) {
            var cc2 = cellsToClear[i2];
            var gg3 = grid[cc2.r][cc2.c];
            if (gg3 && gg3.special === SPECIAL_FLAME) detonations.push({ kind: 'flame', r: cc2.r, c: cc2.c });
            else if (gg3 && gg3.special === SPECIAL_STAR) detonations.push({ kind: 'star', r: cc2.r, c: cc2.c });
        }
        // Expand detonations
        for (var dd = 0; dd < detonations.length; dd++) {
            var det = detonations[dd];
            if (det.kind === 'flame') {
                for (var dr = -1; dr <= 1; dr++) {
                    for (var dc = -1; dc <= 1; dc++) {
                        var rr2 = det.r + dr, cc3 = det.c + dc;
                        if (inBounds(rr2, cc3)) cellsToClear.push({ r: rr2, c: cc3 });
                    }
                }
            } else if (det.kind === 'star') {
                for (var xx = 0; xx < cols; xx++) cellsToClear.push({ r: det.r, c: xx });
                for (var yy = 0; yy < rows; yy++) cellsToClear.push({ r: yy, c: det.c });
            }
        }

        // Deduplicate
        var seen = {};
        var uniq = [];
        for (var u = 0; u < cellsToClear.length; u++) {
            var key = cellsToClear[u].r + ',' + cellsToClear[u].c;
            if (!seen[key]) { seen[key] = true; uniq.push(cellsToClear[u]); }
        }

        // Stagger shatter by distance from match centroid so big groups
        // ripple outward instead of all popping at once.
        var midR = 0, midC = 0;
        for (var sa = 0; sa < uniq.length; sa++) { midR += uniq[sa].r; midC += uniq[sa].c; }
        if (uniq.length > 0) { midR /= uniq.length; midC /= uniq.length; }
        for (var sb = 0; sb < uniq.length; sb++) {
            var dr2 = uniq[sb].r - midR, dc2 = uniq[sb].c - midC;
            uniq[sb]._dist = Math.sqrt(dr2 * dr2 + dc2 * dc2);
        }
        uniq.sort(function (a, b) { return a._dist - b._dist; });
        for (var sc = 0; sc < uniq.length; sc++) {
            uniq[sc].delay = sc * SHATTER_STAGGER_MS;
            uniq[sc].burstFired = false;
        }

        clearTiles(uniq, groups[0].color);

        // Score popup at each match group's centroid (tinted with gem color).
        // Chain banner for x2+.
        if (G.Particles) {
            for (var gi = 0; gi < groups.length; gi++) {
                var grpL = groups[gi];
                var mr = 0, mc = 0;
                for (var ci = 0; ci < grpL.cells.length; ci++) {
                    mr += grpL.cells[ci][0]; mc += grpL.cells[ci][1];
                }
                mr /= grpL.cells.length; mc /= grpL.cells.length;
                var lx = layout.ox + (mc + 0.5) * layout.cell;
                var ly = layout.oy + (mr + 0.5) * layout.cell;
                var labelColor = (PALETTE[grpL.color] && PALETTE[grpL.color].core) || '#ffe9b0';
                var groupScore = baseScore(grpL.size) * Math.max(1, chain);
                G.Particles.popLabel(lx, ly - 6, '+' + groupScore, labelColor, false);
            }
            if (chain >= 2) {
                var bx = layout.ox + layout.boardW / 2;
                var by = layout.oy + layout.boardH / 2;
                G.Particles.popLabel(bx, by, 'CHAIN x' + chain, '#ffe070', true);
            }
        }

        // Mild screen shake escalating with chain (gems should feel weighty).
        shakeAmp = Math.min(5, 1.2 + chain * 0.8);
        shakeT = 200;

        flashDuration = (uniq.length > 0
            ? uniq[uniq.length - 1].delay + SHATTER_MS + 30
            : SHATTER_MS + 30);
        settleTimer = 0;

        pendingFalls = { collapse: true, upgrade: upgrade };
    }

    function clearTiles(tiles, primaryColor) {
        // Defer the actual grid erasure to the flash timer; we only register
        // which tiles should flash.
        flashTiles = tiles.slice();
        flashTimer = 0;
        // timed bonus per clear
        if (mode === 'timed' && tiles.length > 0) {
            modeTimer = Math.min(120000, modeTimer + 200 * tiles.length);
        }
    }

    function collapseAndFill() {
        // for each column, gravity
        for (var c = 0; c < cols; c++) {
            var write = rows - 1;
            for (var r = rows - 1; r >= 0; r--) {
                if (grid[r][c] && !grid[r][c].frozen) {
                    if (r !== write) {
                        grid[write][c] = grid[r][c];
                        grid[r][c] = null;
                        // animate
                        var from = cellXY(r, c);
                        var to = cellXY(write, c);
                        anims.push({ type: 'fall', r: write, c: c, fromY: from.y, toY: to.y, t: 0, dur: FALL_MS });
                    }
                    write--;
                } else if (grid[r][c] && grid[r][c].frozen) {
                    // frozen cells block gravity — next write is above them
                    write = r - 1;
                }
            }
            // fill above with new random
            for (var w = write; w >= 0; w--) {
                if (!grid[w][c]) {
                    var color = 1 + Math.floor(Math.random() * COLORS_N);
                    grid[w][c] = { color: color, special: SPECIAL_NONE, frozen: false };
                    var fromY = layout.oy - (write - w + 1) * layout.cell;
                    anims.push({ type: 'fall', r: w, c: c, fromY: fromY, toY: cellXY(w, c).y, t: 0, dur: FALL_MS });
                }
            }
        }
    }

    function scheduleCollapse() { pendingFalls = { collapse: true }; }

    // ------------------------------------------------------------------
    // HUD
    // ------------------------------------------------------------------

    function updateHUD() {
        var el;
        el = document.getElementById('hud-score'); if (el) el.textContent = String(score);
        el = document.getElementById('hud-level'); if (el) el.textContent = String(level);
        var extraLabel = document.getElementById('hud-extra-label');
        var extraVal = document.getElementById('hud-extra');
        if (mode === 'classic') {
            if (extraLabel) extraLabel.textContent = 'MOVES';
            if (extraVal) extraVal.textContent = String(moves);
        } else if (mode === 'timed') {
            if (extraLabel) extraLabel.textContent = 'TIME';
            var secs = Math.max(0, Math.ceil(modeTimer / 1000));
            if (extraVal) extraVal.textContent = formatTime(modeTimer);
        } else if (mode === 'puzzle') {
            if (extraLabel) extraLabel.textContent = 'FROZEN';
            if (extraVal) extraVal.textContent = String(frozenRemaining);
        }
        var comboLabel = document.getElementById('hud-combo-label');
        var comboVal = document.getElementById('hud-combo');
        if (chain > 1) {
            if (comboLabel) comboLabel.style.display = '';
            if (comboVal) { comboVal.style.display = ''; comboVal.textContent = 'x' + chain; }
        } else {
            if (comboLabel) comboLabel.style.display = 'none';
            if (comboVal) comboVal.style.display = 'none';
        }
    }

    function formatTime(ms) {
        var s = Math.max(0, Math.floor(ms / 1000));
        var m = Math.floor(s / 60);
        s = s % 60;
        return m + ':' + (s < 10 ? '0' : '') + s;
    }

    // ------------------------------------------------------------------
    // Public API
    // ------------------------------------------------------------------

    return {
        // constants
        ROWS: ROWS, COLS: COLS, COLORS_N: COLORS_N,
        SPECIAL_NONE: SPECIAL_NONE,
        SPECIAL_FLAME: SPECIAL_FLAME,
        SPECIAL_STAR: SPECIAL_STAR,
        SPECIAL_HYPER: SPECIAL_HYPER,
        PALETTE: PALETTE,

        // game
        startGame: startGame,
        update: update,
        calcLayout: calcLayout,
        drawBackground: drawBackground,
        drawBoard: drawBoard,
        handleClick: handleClick,
        cursorMove: cursorMove,
        cursorConfirm: cursorConfirm,
        updateHUD: updateHUD,
        formatTime: formatTime,

        // pure helpers / hooks
        findMatches: findMatches,
        findAnyMove: findAnyMove,
        isDeadlocked: isDeadlocked,
        swapMakesMatch: swapMakesMatch,
        seedGrid: seedGrid,
        makeEmptyGrid: makeEmptyGrid,
        scoreChain: scoreChain,

        // introspection
        getGrid: function () { return grid; },
        setGrid: function (g) { grid = g; rows = g.length; cols = g[0].length; },
        getScore: function () { return score; },
        setScore: function (v) { score = v; },
        getMoves: function () { return moves; },
        getLevel: function () { return level; },
        getMode:  function () { return mode; },
        getChain: function () { return chain; },
        getMaxChain: function () { return maxChain; },
        getStats: function () { return stats; },
        getModeTimer: function () { return modeTimer; },
        getFrozenRemaining: function () { return frozenRemaining; },
        isAnimating: function () { return animating; },
        isGameOver: function () { return gameOverFlag; },
        isFinished: function () { return finished; },
        getSelection: function () { return sel; },
        getCursor: function () { return cursor; },
        getLayout: function () { return layout; },
        setHintDelay: function (ms) { hintDelay = ms; },
        clearHint: function () { hint = null; idleTimer = 0; },
        cellXY: cellXY,
    };
})();
