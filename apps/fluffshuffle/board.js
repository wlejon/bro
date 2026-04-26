// board.js — Fluffshuffle board state, wrap-drag mechanics, match detection,
// cascades, scoring, and rendering.
'use strict';
var G = G || {};

G.Board = (function () {
    var ROWS = 6;
    var COLS = 6;
    var COLORS_N = 6;

    // Specials — stored on a puff cell. 0 = none.
    var SPECIAL_NONE   = 0;
    var SPECIAL_JUMBO  = 1;   // match 4 line: 3x3 clear
    var SPECIAL_ARROW  = 2;   // match 5 line: row or column clear
    var SPECIAL_PRISM  = 3;   // L/T shape: clear all of matched color

    // Grid state: grid[r][c] = { color:1..6, special:0..3, locked:bool,
    //                            phase:<rad>, blinkOffset:<ms>, arrowDir?:'h'|'v' }
    var grid = [];
    var rows = ROWS, cols = COLS;

    // Layout / rendering anchors set once per frame by calcLayout().
    var layout = { ox: 0, oy: 0, cell: 96, boardW: 0, boardH: 0 };

    // Gameplay state.
    var score = 0;
    var level = 1;
    var mode = 'classic';
    var popped = 0;            // total puffs popped (drives level-up)
    var moves = 0;
    var modeTimer = 0;         // ms remaining for timed mode
    var puzzleIndex = 0;
    var puzzleMovesLeft = 0;
    var puzzleTarget = 0;
    var chain = 0;
    var maxChain = 0;
    var gameTime = 0;
    var finished = false;
    var gameOverFlag = false;
    var stats = null;

    // Drag-in-progress state (live preview of a wrap-shift).
    // drag = { axis:'h'|'v', index:<row or col>, offsetPx:<continuous> }
    var drag = null;

    // Cursor (keyboard navigation).
    var cursor = { r: 3, c: 3, active: false };

    // Pending post-shift animation: snap-tween from current offsetPx down to
    // the cell-aligned offset, then apply.
    // snap = { axis, index, startPx, cells, startTime, dur }
    var snap = null;

    // Gravity / spawn animation: cells with { pending:true, fromY } until
    // the tween finishes and the grid presents normally.
    var anims = [];            // active fall animations

    // Flash-then-clear sequence after a match.
    var flashTiles = [];       // [{r,c,kind,color}]
    var flashTimer = 0;
    var FLASH_MS = 260;          // updated per-cascade based on group size
    var FALL_MS  = 140;
    var SNAP_MS  = 140;
    var POP_DUR_MS = 140;        // per-tile pop animation length
    var POP_STAGGER_MS = 32;     // delay between tiles popping in a group
    var SETTLE_MS = 40;         // breath between cascades

    var flashDuration = FLASH_MS;
    var settleTimer = 0;

    // Last-cascade upgrades to place after flash clears.
    var pendingUpgrades = null;

    // Lock-spawn cadence for classic mode.
    var lockTimer = 0;

    // Idle shake when cascades happen.
    var shakeAmp = 0;
    var shakeT = 0;

    // Mouse-tracked world position (used for puff eyes).
    var pointer = { x: -9999, y: -9999 };

    // Stable PRNG — optional seed via setSeed(); otherwise Math.random.
    var rngState = 0;
    function rnd() {
        if (rngState === 0) return Math.random();
        rngState = (rngState * 1664525 + 1013904223) >>> 0;
        return rngState / 0x100000000;
    }
    function setSeed(s) { rngState = (s | 0) || 1; }

    // ----------------------------------------------------------------------
    // Pure helpers (testable from outside).
    // ----------------------------------------------------------------------

    function inBounds(r, c) { return r >= 0 && r < rows && c >= 0 && c < cols; }
    function cellColor(g) { return g ? g.color : 0; }

    function makeEmptyGrid() {
        var g = [];
        for (var r = 0; r < rows; r++) {
            var row = [];
            for (var c = 0; c < cols; c++) row.push(null);
            g.push(row);
        }
        return g;
    }

    function makePuff(color, special, locked) {
        return {
            color: color,
            special: special || 0,
            locked: !!locked,
            phase: rnd() * Math.PI * 2,
            blinkOffset: Math.floor(rnd() * 3200),
            arrowDir: null,
        };
    }

    function copyGrid(g) {
        var out = [];
        for (var r = 0; r < g.length; r++) {
            var row = [];
            for (var c = 0; c < g[r].length; c++) {
                row.push(g[r][c] ? { color: g[r][c].color, special: g[r][c].special || 0,
                                     locked: !!g[r][c].locked,
                                     phase: g[r][c].phase || 0,
                                     blinkOffset: g[r][c].blinkOffset || 0,
                                     arrowDir: g[r][c].arrowDir || null } : null);
            }
            out.push(row);
        }
        return out;
    }

    // Pure: slide row `r` in grid `g` by `offset` cells (positive = right),
    // with wrap. Returns a NEW grid (does not mutate).
    function slideRow(g, r, offset) {
        var out = copyGrid(g);
        if (!out[r]) return out;
        var w = out[r].length;
        var k = ((offset % w) + w) % w;
        if (k === 0) return out;
        var row = out[r];
        var shifted = new Array(w);
        for (var c = 0; c < w; c++) {
            shifted[(c + k) % w] = row[c];
        }
        out[r] = shifted;
        return out;
    }

    // Pure: slide column `c` by `offset` cells (positive = down), wrap.
    function slideCol(g, c, offset) {
        var out = copyGrid(g);
        var h = out.length;
        var k = ((offset % h) + h) % h;
        if (k === 0) return out;
        var col = new Array(h);
        for (var r = 0; r < h; r++) col[r] = out[r][c];
        var shifted = new Array(h);
        for (var r2 = 0; r2 < h; r2++) {
            shifted[(r2 + k) % h] = col[r2];
        }
        for (var r3 = 0; r3 < h; r3++) out[r3][c] = shifted[r3];
        return out;
    }

    // Find all matches in `g`. A match is 3+ same color horizontally or
    // vertically (no wrap in matching — the wrap only applies during the
    // shift itself, the committed board is read linearly). Returns groups
    // merged by connectivity — L/T shapes fuse into a single group.
    function findMatches(g) {
        var hRuns = [];
        var vRuns = [];
        var r, c, color, start, run;
        for (r = 0; r < rows; r++) {
            c = 0;
            while (c < cols) {
                color = cellColor(g[r][c]);
                if (!color) { c++; continue; }
                start = c;
                while (c < cols && cellColor(g[r][c]) === color) c++;
                run = c - start;
                if (run >= 3) hRuns.push({ r: r, c: start, len: run, horiz: true });
            }
        }
        for (c = 0; c < cols; c++) {
            r = 0;
            while (r < rows) {
                color = cellColor(g[r][c]);
                if (!color) { r++; continue; }
                start = r;
                while (r < rows && cellColor(g[r][c]) === color) r++;
                run = r - start;
                if (run >= 3) vRuns.push({ r: start, c: c, len: run, horiz: false });
            }
        }
        var all = hRuns.concat(vRuns);
        if (all.length === 0) return [];

        function runCells(run) {
            var out = [];
            for (var k = 0; k < run.len; k++) {
                if (run.horiz) out.push([run.r, run.c + k]);
                else out.push([run.r + k, run.c]);
            }
            return out;
        }
        var parent = new Array(all.length);
        for (var i = 0; i < all.length; i++) parent[i] = i;
        function find(i) { while (parent[i] !== i) { parent[i] = parent[parent[i]]; i = parent[i]; } return i; }
        function union(a, b) { a = find(a); b = find(b); if (a !== b) parent[a] = b; }
        // Merge runs of same color that share a cell.
        for (var i2 = 0; i2 < all.length; i2++) {
            for (var j = i2 + 1; j < all.length; j++) {
                if (cellColor(g[all[i2].r][all[i2].c]) !== cellColor(g[all[j].r][all[j].c])) continue;
                var ci = runCells(all[i2]);
                var cj = runCells(all[j]);
                var shared = false;
                for (var a = 0; a < ci.length && !shared; a++) {
                    for (var b = 0; b < cj.length && !shared; b++) {
                        if (ci[a][0] === cj[b][0] && ci[a][1] === cj[b][1]) shared = true;
                    }
                }
                if (shared) union(i2, j);
            }
        }
        var buckets = {};
        for (var i3 = 0; i3 < all.length; i3++) {
            var root = find(i3);
            if (!buckets[root]) buckets[root] = { runs: [], cells: {} };
            buckets[root].runs.push(all[i3]);
            var cells = runCells(all[i3]);
            for (var k2 = 0; k2 < cells.length; k2++) {
                var key = cells[k2][0] + ',' + cells[k2][1];
                buckets[root].cells[key] = cells[k2];
            }
        }
        var groups = [];
        for (var rk in buckets) {
            if (!Object.prototype.hasOwnProperty.call(buckets, rk)) continue;
            var bk = buckets[rk];
            var cellList = [];
            for (var k3 in bk.cells) cellList.push(bk.cells[k3]);
            var hasH = false, hasV = false, lensH = 0, lensV = 0;
            for (var rr = 0; rr < bk.runs.length; rr++) {
                var rn = bk.runs[rr];
                if (rn.horiz) { hasH = true; lensH = Math.max(lensH, rn.len); }
                else          { hasV = true; lensV = Math.max(lensV, rn.len); }
            }
            var maxLine = Math.max(lensH, lensV);
            var special = SPECIAL_NONE;
            var arrowDir = null;
            if (hasH && hasV) {
                special = SPECIAL_PRISM;
            } else if (maxLine >= 5) {
                special = SPECIAL_ARROW;
                arrowDir = hasH ? 'h' : 'v';
            } else if (maxLine === 4) {
                special = SPECIAL_JUMBO;
            }
            var color = cellColor(g[bk.runs[0].r][bk.runs[0].c]);
            groups.push({
                cells: cellList, color: color, special: special, arrowDir: arrowDir,
                size: cellList.length, maxLine: maxLine,
                hasH: hasH, hasV: hasV,
            });
        }
        return groups;
    }

    // Would any horizontal row-shift (by 1..cols-1) or column-shift produce
    // a match? Returns the list of legal (axis, index, k) shifts.
    function legalShifts(g) {
        var out = [];
        var r, c, k;
        for (r = 0; r < rows; r++) {
            // Skip rows that contain any locked puff — they can't shift.
            var rowLocked = false;
            for (c = 0; c < cols; c++) if (g[r][c] && g[r][c].locked) { rowLocked = true; break; }
            if (rowLocked) continue;
            for (k = 1; k < cols; k++) {
                var sg = slideRow(g, r, k);
                if (findMatches(sg).length > 0) { out.push({ axis: 'h', index: r, k: k }); break; }
            }
        }
        for (c = 0; c < cols; c++) {
            var colLocked = false;
            for (r = 0; r < rows; r++) if (g[r][c] && g[r][c].locked) { colLocked = true; break; }
            if (colLocked) continue;
            for (k = 1; k < rows; k++) {
                var sg2 = slideCol(g, c, k);
                if (findMatches(sg2).length > 0) { out.push({ axis: 'v', index: c, k: k }); break; }
            }
        }
        return out;
    }

    function hasAnyMatchingShift(g) { return legalShifts(g).length > 0; }

    // Seed a grid: random colors, no pre-existing matches, at least one
    // legal shift exists.
    function seedGrid(seedRand) {
        var rand = seedRand || rnd;
        var attempts = 0;
        while (attempts++ < 200) {
            var g = makeEmptyGrid();
            for (var r = 0; r < rows; r++) {
                for (var c = 0; c < cols; c++) {
                    var tries = 0, color = 1;
                    while (tries++ < 40) {
                        color = 1 + Math.floor(rand() * COLORS_N);
                        if (c >= 2 && g[r][c-1] && g[r][c-2] &&
                            g[r][c-1].color === color && g[r][c-2].color === color) continue;
                        if (r >= 2 && g[r-1][c] && g[r-2][c] &&
                            g[r-1][c].color === color && g[r-2][c].color === color) continue;
                        break;
                    }
                    g[r][c] = makePuff(color, 0, false);
                }
            }
            if (findMatches(g).length === 0 && hasAnyMatchingShift(g)) return g;
        }
        return g;
    }

    // Scoring helper — per-puff base * cascade multiplier (chainDepth+1).
    function scoreChain(cellCount, chainDepth) {
        return cellCount * 50 * (chainDepth + 1);
    }

    // ----------------------------------------------------------------------
    // Game flow
    // ----------------------------------------------------------------------

    function startGame(selectedMode) {
        mode = selectedMode || 'classic';
        score = 0;
        level = 1;
        popped = 0;
        moves = 0;
        chain = 0;
        maxChain = 0;
        gameTime = 0;
        finished = false;
        gameOverFlag = false;
        drag = null;
        snap = null;
        anims = [];
        flashTiles = [];
        pendingUpgrades = null;
        shakeAmp = 0;
        shakeT = 0;
        lockTimer = 0;
        settleTimer = 0;
        flashDuration = FLASH_MS;
        cursor = { r: Math.floor(rows / 2), c: Math.floor(cols / 2), active: false };
        stats = { moves: 0, popped: 0, jumboMade: 0, arrowMade: 0, prismMade: 0, maxChain: 0, unlocks: 0 };

        if (mode === 'timed') {
            modeTimer = 120000;
        } else if (mode === 'puzzle') {
            puzzleIndex = 0;
            loadPuzzle(puzzleIndex);
            return;
        }
        grid = seedGrid();
    }

    // Procedurally generate puzzles on demand to fulfil the "20 layouts" goal.
    function loadPuzzle(idx) {
        // Seeded per puzzle for reproducibility.
        setSeed(0x5afe00 + idx * 7919);
        grid = seedGrid(rnd);
        setSeed(0);

        // Sprinkle locked puffs — count rises with puzzle index.
        var locks = Math.min(5, 1 + Math.floor(idx / 3));
        for (var i = 0; i < locks; i++) {
            var r = Math.floor(Math.random() * rows);
            var c = Math.floor(Math.random() * cols);
            if (grid[r][c] && !grid[r][c].locked) grid[r][c].locked = true;
        }
        // Target: clear N puffs in M moves.
        puzzleTarget = 16 + idx * 2;
        puzzleMovesLeft = 10 + Math.floor(idx / 2);
        popped = 0;
    }

    function nextPuzzle() {
        puzzleIndex++;
        if (puzzleIndex >= 20) {
            finished = true;
            return false;
        }
        loadPuzzle(puzzleIndex);
        return true;
    }

    // ----------------------------------------------------------------------
    // Layout / coord helpers
    // ----------------------------------------------------------------------

    function calcLayout(W, H) {
        var maxBoard = Math.min(W - 200, H - 80);
        var cell = Math.floor(maxBoard / cols);
        if (cell < 48) cell = 48;
        if (cell > 100) cell = 100;
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

    function pointInBoard(px, py) {
        return px >= layout.ox && py >= layout.oy
            && px < layout.ox + layout.boardW
            && py < layout.oy + layout.boardH;
    }

    // ----------------------------------------------------------------------
    // Drawing
    // ----------------------------------------------------------------------

    function drawBackground(ctx, W, H) {
        var g = ctx.createLinearGradient(0, 0, 0, H);
        g.addColorStop(0, '#0d1326');
        g.addColorStop(1, '#231a38');
        ctx.fillStyle = g;
        ctx.fillRect(0, 0, W, H);
        // Drifting dust.
        var t = gameTime * 0.00008;
        for (var i = 0; i < 50; i++) {
            var x = ((i * 97.31 + t * W * 2) % W);
            var y = ((i * 173.7 + t * H) % H);
            var s = 0.4 + (i % 5) * 0.2;
            ctx.fillStyle = 'rgba(255, 200, 220, ' + (0.05 + (i % 3) * 0.03) + ')';
            ctx.fillRect(x, y, s, s);
        }
    }

    // Hovered cell when the player isn't actively dragging or animating.
    // Used to preview which row/column they'd move and to flag lock blockers.
    function hoverCell() {
        if (drag || snap || flashTiles.length > 0 || gameOverFlag) return null;
        if (!pointInBoard(pointer.x, pointer.y)) return null;
        return pointToCell(pointer.x, pointer.y);
    }

    function rowHasLock(r) {
        for (var c = 0; c < cols; c++) if (grid[r][c] && grid[r][c].locked) return true;
        return false;
    }
    function colHasLock(c) {
        for (var r = 0; r < rows; r++) if (grid[r][c] && grid[r][c].locked) return true;
        return false;
    }

    function drawBoardFrame(ctx) {
        var sx = 0, sy = 0;
        if (shakeAmp > 0) {
            sx = (Math.random() - 0.5) * shakeAmp;
            sy = (Math.random() - 0.5) * shakeAmp;
        }
        ctx.save();
        ctx.translate(sx, sy);

        var x = layout.ox - 10, y = layout.oy - 10;
        var w = layout.boardW + 20, h = layout.boardH + 20;
        ctx.fillStyle = 'rgba(26, 36, 60, 0.72)';
        ctx.fillRect(x, y, w, h);
        ctx.strokeStyle = '#3a4a70';
        ctx.lineWidth = 2;
        ctx.strokeRect(x + 0.5, y + 0.5, w - 1, h - 1);

        var hov = hoverCell();
        var hovRowBlocked = hov && rowHasLock(hov.r);
        var hovColBlocked = hov && colHasLock(hov.c);

        // Cell tint. Highlight the active row/column while dragging, and
        // the hovered row+column when idle (red if locked, soft green if free).
        for (var r = 0; r < rows; r++) {
            for (var c = 0; c < cols; c++) {
                var p = cellXY(r, c);
                var checker = (r + c) & 1;
                var hot = drag &&
                    ((drag.axis === 'h' && drag.index === r) ||
                     (drag.axis === 'v' && drag.index === c));
                var fill;
                if (hot) {
                    fill = checker ? 'rgba(70, 90, 140, 0.55)' : 'rgba(60, 80, 130, 0.55)';
                } else if (hov && (r === hov.r || c === hov.c)) {
                    var rowHit = (r === hov.r);
                    var colHit = (c === hov.c);
                    var blocked = (rowHit && hovRowBlocked) || (colHit && hovColBlocked);
                    if (blocked) {
                        fill = checker ? 'rgba(120, 50, 60, 0.55)' : 'rgba(105, 42, 52, 0.55)';
                    } else {
                        fill = checker ? 'rgba(50, 90, 80, 0.55)' : 'rgba(42, 80, 70, 0.55)';
                    }
                } else {
                    fill = checker ? 'rgba(30, 40, 70, 0.55)' : 'rgba(22, 32, 60, 0.55)';
                }
                ctx.fillStyle = fill;
                ctx.fillRect(p.x, p.y, layout.cell, layout.cell);
            }
        }

        // Gutter stripes — permanent indicator of locked rows/columns.
        // Drawn in the 10px gutter between the board edge and the frame.
        var lockColor = 'rgba(180, 220, 255, 0.78)';
        var lockColorHi = 'rgba(220, 240, 255, 0.95)';
        for (var rr = 0; rr < rows; rr++) {
            if (!rowHasLock(rr)) continue;
            var py = layout.oy + rr * layout.cell + 2;
            var ph = layout.cell - 4;
            var hi = hov && hov.r === rr;
            ctx.fillStyle = hi ? lockColorHi : lockColor;
            ctx.fillRect(layout.ox - 7, py, 4, ph);
            ctx.fillRect(layout.ox + layout.boardW + 3, py, 4, ph);
        }
        for (var cc = 0; cc < cols; cc++) {
            if (!colHasLock(cc)) continue;
            var px = layout.ox + cc * layout.cell + 2;
            var pw = layout.cell - 4;
            var hi2 = hov && hov.c === cc;
            ctx.fillStyle = hi2 ? lockColorHi : lockColor;
            ctx.fillRect(px, layout.oy - 7, pw, 4);
            ctx.fillRect(px, layout.oy + layout.boardH + 3, pw, 4);
        }

        ctx.restore();
    }

    // Compute the continuous offset for a tile currently involved in the
    // drag (used for live wrap preview and snap tween).
    function tileDragOffset(r, c) {
        if (snap) {
            var t = Math.min(1, snap.t / snap.dur);
            var ease = 1 - Math.pow(1 - t, 3);
            var offPx = snap.startPx + (snap.targetPx - snap.startPx) * ease;
            if (snap.axis === 'h' && snap.index === r) return { dx: offPx, dy: 0 };
            if (snap.axis === 'v' && snap.index === c) return { dx: 0, dy: offPx };
        }
        if (drag) {
            if (drag.axis === 'h' && drag.index === r) return { dx: drag.offsetPx, dy: 0 };
            if (drag.axis === 'v' && drag.index === c) return { dx: 0, dy: drag.offsetPx };
        }
        return { dx: 0, dy: 0 };
    }

    function drawBoard(ctx) {
        drawBoardFrame(ctx);

        var sx = 0, sy = 0;
        if (shakeAmp > 0) {
            sx = (Math.random() - 0.5) * shakeAmp;
            sy = (Math.random() - 0.5) * shakeAmp;
        }
        ctx.save();
        ctx.translate(sx, sy);

        // Clip to board so wrap-preview bleeds are hidden outside the frame.
        ctx.save();
        ctx.beginPath();
        ctx.rect(layout.ox, layout.oy, layout.boardW, layout.boardH);
        ctx.clip();

        var cell = layout.cell;

        // Build a pop-state lookup so the grid-draw loop can render popping
        // tiles with anticipation/burst rather than as plain idle puffs.
        var popMap = null;
        if (flashTiles.length > 0) {
            popMap = {};
            for (var fpi = 0; fpi < flashTiles.length; fpi++) {
                var fpt = flashTiles[fpi];
                popMap[fpt.r + ',' + fpt.c] = fpt;
            }
        }

        // Draw each puff. For tiles on the active drag axis, render them
        // twice (original + wrapped copy offset by ±boardW/H) so the slide
        // reads as seamless.
        for (var r = 0; r < rows; r++) {
            for (var c = 0; c < cols; c++) {
                var g = grid[r][c];
                if (!g) continue;
                var p = cellXY(r, c);
                var cx = p.x + cell / 2;
                var cy = p.y + cell / 2;
                var off = tileDragOffset(r, c);
                var fall = fallOffset(r, c);

                var bx = cx + off.dx;
                var by = cy + off.dy + fall;

                var popOpts = null;
                if (popMap) {
                    var pop = popMap[r + ',' + c];
                    if (pop) {
                        var local = (flashTimer - pop.delay);
                        if (local < 0) {
                            // Anticipation — slight pulse before this tile pops.
                            var pre = Math.max(0, 1 + local / 80);
                            popOpts = { pulse: 0.10 * pre };
                        } else if (local < POP_DUR_MS) {
                            popOpts = { popLocal: local / POP_DUR_MS };
                        } else {
                            popOpts = { skip: true };
                        }
                    }
                }
                if (popOpts && popOpts.skip) continue;

                drawPuffAt(ctx, g, bx, by, cell, heldTile(r, c), popOpts);
                if (off.dx !== 0) {
                    drawPuffAt(ctx, g, bx + layout.boardW, by, cell, heldTile(r, c), popOpts);
                    drawPuffAt(ctx, g, bx - layout.boardW, by, cell, heldTile(r, c), popOpts);
                }
                if (off.dy !== 0) {
                    drawPuffAt(ctx, g, bx, by + layout.boardH, cell, heldTile(r, c), popOpts);
                    drawPuffAt(ctx, g, bx, by - layout.boardH, cell, heldTile(r, c), popOpts);
                }
            }
        }

        // Hover blocker halo — when the player is hovering a cell whose
        // row or column contains a locked puff, ring the locking puff(s)
        // so the source of the restriction is obvious.
        var hov2 = hoverCell();
        if (hov2) {
            var pulse = 0.55 + 0.45 * Math.sin(gameTime * 0.006);
            ctx.strokeStyle = 'rgba(255, 200, 210, ' + (0.45 + pulse * 0.35) + ')';
            ctx.lineWidth = 2.2;
            for (var rL = 0; rL < rows; rL++) {
                for (var cL = 0; cL < cols; cL++) {
                    var gL = grid[rL][cL];
                    if (!gL || !gL.locked) continue;
                    if (rL !== hov2.r && cL !== hov2.c) continue;
                    var pL = cellXY(rL, cL);
                    var cxL = pL.x + cell / 2, cyL = pL.y + cell / 2;
                    ctx.beginPath();
                    ctx.arc(cxL, cyL, cell * 0.42, 0, Math.PI * 2);
                    ctx.stroke();
                }
            }
        }

        // Pop shock ring per tile — expands from the tile and fades, syncing
        // with that tile's individual pop rather than the whole flash window.
        if (flashTiles.length > 0) {
            for (var i = 0; i < flashTiles.length; i++) {
                var t = flashTiles[i];
                var localR = (flashTimer - t.delay) / POP_DUR_MS;
                if (localR <= 0 || localR >= 1) continue;
                var p2 = cellXY(t.r, t.c);
                var cx2 = p2.x + cell / 2, cy2 = p2.y + cell / 2;
                var fade = 1 - localR;
                ctx.strokeStyle = 'rgba(255, 240, 200, ' + (fade * 0.85) + ')';
                ctx.lineWidth = 3;
                ctx.beginPath();
                ctx.arc(cx2, cy2, cell * 0.35 + localR * cell * 0.55, 0, Math.PI * 2);
                ctx.stroke();
            }
        }

        ctx.restore();

        // Cursor (drawn outside the clip, above everything board-related).
        if (cursor.active) {
            var p3 = cellXY(cursor.r, cursor.c);
            var pulse = 0.5 + 0.5 * Math.sin(gameTime * 0.008);
            ctx.strokeStyle = 'rgba(255, 240, 180, ' + (0.5 + pulse * 0.5) + ')';
            ctx.lineWidth = 3;
            ctx.strokeRect(p3.x + 2.5, p3.y + 2.5, cell - 5, cell - 5);
        }

        ctx.restore();
    }

    function fallOffset(r, c) {
        for (var i = 0; i < anims.length; i++) {
            var an = anims[i];
            if (an.type === 'fall' && an.r === r && an.c === c) {
                var t = Math.min(1, an.t / an.dur);
                var ease = 1 - Math.pow(1 - t, 3);
                return (an.fromOffset) * (1 - ease);
            }
            if (an.type === 'pop' && an.r === r && an.c === c) {
                // Popped cells are drawn from flashTiles, not from grid.
            }
        }
        return 0;
    }

    function drawPuffAt(ctx, puff, cx, cy, cell, held, popOpts) {
        if (!puff) return;
        var lookAt = (G.Screens && G.Screens.settings && G.Screens.settings().eyeTrack)
            ? pointer : null;
        var state = held ? 'held' : 'idle';
        var pulse = 0;
        var popProgress = 0;
        var alpha = 1;
        if (popOpts) {
            if (popOpts.popLocal != null) {
                state = 'pop';
                popProgress = popOpts.popLocal;
                alpha = 1 - popOpts.popLocal;
            } else if (popOpts.pulse != null) {
                pulse = popOpts.pulse;
            }
        }
        var opts = {
            t: gameTime,
            lookAt: lookAt,
            state: state,
            popProgress: popProgress,
            pulse: pulse,
        };
        if (alpha < 1) {
            ctx.save();
            ctx.globalAlpha = Math.max(0, alpha);
            G.Puffs.draw(ctx, puff, cx, cy, cell, opts);
            ctx.restore();
        } else {
            G.Puffs.draw(ctx, puff, cx, cy, cell, opts);
        }
    }

    function heldTile(r, c) {
        if (!drag) return false;
        if (drag.axis === 'h' && drag.index === r) return true;
        if (drag.axis === 'v' && drag.index === c) return true;
        return false;
    }

    // ----------------------------------------------------------------------
    // Input — wrap drag
    // ----------------------------------------------------------------------

    function handleMouseDown(px, py) {
        pointer.x = px; pointer.y = py;
        if (!pointInBoard(px, py)) return;
        if (isBusy()) return;
        var cell = pointToCell(px, py);
        if (!cell) return;
        if (!grid[cell.r][cell.c]) return;
        drag = {
            axis: null,
            index: -1,
            startR: cell.r,
            startC: cell.c,
            startX: px,
            startY: py,
            offsetPx: 0,
            moved: false,
        };
        if (G.AppAudio) G.AppAudio.grab();
    }

    function handleMouseMove(px, py) {
        pointer.x = px; pointer.y = py;
        if (!drag) return;
        var dx = px - drag.startX;
        var dy = py - drag.startY;
        if (drag.axis === null) {
            var dead = (G.Screens && G.Screens.settings && G.Screens.settings().dragDead) || 6;
            if (Math.abs(dx) >= dead || Math.abs(dy) >= dead) {
                drag.axis  = Math.abs(dx) >= Math.abs(dy) ? 'h' : 'v';
                drag.index = drag.axis === 'h' ? drag.startR : drag.startC;
                // Lock check: if this row/column contains a locked puff,
                // cancel the drag with a thud.
                if (rowOrColLocked(drag.axis, drag.index)) {
                    if (G.AppAudio) G.AppAudio.thud();
                    drag = null;
                    return;
                }
            } else {
                return;
            }
        }
        drag.offsetPx = drag.axis === 'h' ? dx : dy;
        drag.moved = true;
    }

    function handleMouseUp(px, py) {
        pointer.x = px; pointer.y = py;
        if (!drag) return;
        if (!drag.moved || drag.axis === null) { drag = null; return; }
        commitDrag();
    }

    function rowOrColLocked(axis, index) {
        return axis === 'h' ? rowHasLock(index) : colHasLock(index);
    }

    function commitDrag() {
        var d = drag;
        var cellPx = layout.cell;
        var cellsRaw = d.offsetPx / cellPx;
        var cells = Math.round(cellsRaw);
        // Snap from current continuous position to the aligned cell offset.
        var targetPx = cells * cellPx;
        snap = {
            axis: d.axis, index: d.index,
            startPx: d.offsetPx,
            targetPx: targetPx,
            t: 0, dur: SNAP_MS,
            cells: cells,
        };
        drag = null;
        if (G.AppAudio) G.AppAudio.snap();
    }

    function applySnap() {
        var cells = snap.cells;
        var axis  = snap.axis;
        var index = snap.index;
        snap = null;
        if (cells !== 0) {
            if (axis === 'h') grid = slideRow(grid, index, cells);
            else              grid = slideCol(grid, index, cells);
            moves++;
            stats.moves++;
            if (mode === 'puzzle') puzzleMovesLeft--;
            chain = 0;
            resolveMatches();
        }
    }

    // ----------------------------------------------------------------------
    // Keyboard cursor controls
    // ----------------------------------------------------------------------

    // Cursor navigation when NOT grabbing.
    function cursorMove(dr, dc) {
        cursor.active = true;
        cursor.r = ((cursor.r + dr) % rows + rows) % rows;
        cursor.c = ((cursor.c + dc) % cols + cols) % cols;
        if (G.AppAudio) G.AppAudio.cursor();
    }

    // Space to grab/release; when grabbed, arrow keys slide.
    function cursorAction() {
        if (isBusy()) return;
        if (drag && drag.axis !== null) {
            // release → commit
            commitDrag();
            return;
        }
        // Begin a grab at the cursor.
        if (!grid[cursor.r][cursor.c]) return;
        drag = {
            axis: null,
            index: -1,
            startR: cursor.r,
            startC: cursor.c,
            startX: 0, startY: 0,
            offsetPx: 0,
            moved: false,
        };
        if (G.AppAudio) G.AppAudio.grab();
    }

    function cursorSlide(dr, dc) {
        if (!drag) { cursorMove(dr, dc); return; }
        // Lock the axis based on the first slide direction.
        if (drag.axis === null) {
            if (dc !== 0) { drag.axis = 'h'; drag.index = drag.startR; }
            else          { drag.axis = 'v'; drag.index = drag.startC; }
            if (rowOrColLocked(drag.axis, drag.index)) {
                if (G.AppAudio) G.AppAudio.thud();
                drag = null;
                return;
            }
        }
        var d = (drag.axis === 'h' ? dc : dr) * layout.cell;
        drag.offsetPx += d;
        drag.moved = true;
    }

    // ----------------------------------------------------------------------
    // Match resolution + cascades
    // ----------------------------------------------------------------------

    function resolveMatches() {
        var groups = findMatches(grid);
        if (groups.length === 0) {
            // Still animated? Let update() idle-check the deadlock.
            return;
        }
        resolveGroups(groups);
    }

    function resolveGroups(groups) {
        chain++;
        if (chain > maxChain) maxChain = chain;
        stats.maxChain = maxChain;

        // Sum puffs cleared (used for scoring + level).
        var count = 0;
        for (var i = 0; i < groups.length; i++) count += groups[i].size;
        var delta = scoreChain(count, chain - 1);
        score += delta;
        popped += count;
        stats.popped += count;

        if (G.AppAudio) G.AppAudio.match(chain, groups[0].color, groups[0].size);
        shakeAmp = Math.min(6, chain * 1.5);
        shakeT = 180;

        // Collect tiles to clear, plus upgrade sites (where a special spawns).
        var toClear = [];
        var upgrades = [];
        for (var g = 0; g < groups.length; g++) {
            var grp = groups[g];
            // Special detonations: if any cleared tile holds a special, expand.
            for (var k = 0; k < grp.cells.length; k++) {
                toClear.push({ r: grp.cells[k][0], c: grp.cells[k][1], color: grp.color });
            }
            if (grp.special !== SPECIAL_NONE) {
                // Pick the centroid cell as the upgrade site.
                var cx = grp.cells[Math.floor(grp.cells.length / 2)];
                upgrades.push({ r: cx[0], c: cx[1], special: grp.special, color: grp.color,
                                arrowDir: grp.arrowDir });
                if (grp.special === SPECIAL_JUMBO) stats.jumboMade++;
                else if (grp.special === SPECIAL_ARROW) stats.arrowMade++;
                else if (grp.special === SPECIAL_PRISM) stats.prismMade++;
            }
        }

        // Expand detonations: any cleared cell with a pre-existing special
        // triggers its effect (which adds more cells to clear).
        var extraClears = [];
        for (var ii = 0; ii < toClear.length; ii++) {
            var tc = toClear[ii];
            var cellRef = grid[tc.r][tc.c];
            if (!cellRef) continue;
            if (cellRef.special === SPECIAL_JUMBO) {
                for (var dr = -1; dr <= 1; dr++) for (var dc = -1; dc <= 1; dc++) {
                    if (inBounds(tc.r + dr, tc.c + dc)) extraClears.push({ r: tc.r + dr, c: tc.c + dc, color: 0 });
                }
            } else if (cellRef.special === SPECIAL_ARROW) {
                if (cellRef.arrowDir === 'h') {
                    for (var c1 = 0; c1 < cols; c1++) extraClears.push({ r: tc.r, c: c1, color: 0 });
                } else {
                    for (var r1 = 0; r1 < rows; r1++) extraClears.push({ r: r1, c: tc.c, color: 0 });
                }
            } else if (cellRef.special === SPECIAL_PRISM) {
                var targetColor = cellRef.color;
                for (var rr = 0; rr < rows; rr++) {
                    for (var cc = 0; cc < cols; cc++) {
                        if (grid[rr][cc] && grid[rr][cc].color === targetColor) {
                            extraClears.push({ r: rr, c: cc, color: targetColor });
                        }
                    }
                }
            }
        }
        toClear = toClear.concat(extraClears);

        // Dedup clears; also unlock locked puffs that are in a match set.
        var seen = {};
        var uniq = [];
        for (var u = 0; u < toClear.length; u++) {
            var key = toClear[u].r + ',' + toClear[u].c;
            if (!seen[key]) { seen[key] = true; uniq.push(toClear[u]); }
        }

        // Exclude upgrade sites from clearance (the cell becomes the special
        // rather than vanishing), but only when that upgrade is actually going
        // to be placed there (spot must be one of the match cells).
        var upgradeSet = {};
        for (var ug = 0; ug < upgrades.length; ug++) {
            upgradeSet[upgrades[ug].r + ',' + upgrades[ug].c] = upgrades[ug];
        }

        var finalClears = [];
        for (var fc = 0; fc < uniq.length; fc++) {
            var u2 = uniq[fc];
            var keyU = u2.r + ',' + u2.c;
            if (upgradeSet[keyU]) continue;
            finalClears.push(u2);
        }

        // Queue flash with a per-tile stagger so a big group ripples instead
        // of popping all at once. Tiles further from the centroid pop later.
        var staged = finalClears.slice();
        var cxAvg = 0, cyAvg = 0;
        for (var sa = 0; sa < staged.length; sa++) { cxAvg += staged[sa].r; cyAvg += staged[sa].c; }
        if (staged.length > 0) { cxAvg /= staged.length; cyAvg /= staged.length; }
        for (var sb = 0; sb < staged.length; sb++) {
            var dr2 = staged[sb].r - cxAvg, dc2 = staged[sb].c - cyAvg;
            staged[sb]._dist = Math.sqrt(dr2 * dr2 + dc2 * dc2);
        }
        staged.sort(function (a, b) { return a._dist - b._dist; });
        for (var sc2 = 0; sc2 < staged.length; sc2++) {
            staged[sc2].delay = sc2 * POP_STAGGER_MS;
            staged[sc2].burstFired = false;
        }
        flashTiles = staged;
        flashTimer = 0;
        flashDuration = (staged.length > 0
            ? staged[staged.length - 1].delay + POP_DUR_MS + 30
            : POP_DUR_MS + 30);
        pendingUpgrades = upgrades;
        settleTimer = 0;

        // Score popup at each match group's centroid (escalates with chain).
        if (G.Particles) {
            var PAL2 = G.Puffs.PALETTE;
            for (var gi = 0; gi < groups.length; gi++) {
                var grpL = groups[gi];
                var midR = 0, midC = 0;
                for (var ci = 0; ci < grpL.cells.length; ci++) {
                    midR += grpL.cells[ci][0]; midC += grpL.cells[ci][1];
                }
                midR /= grpL.cells.length; midC /= grpL.cells.length;
                var lx = layout.ox + (midC + 0.5) * layout.cell;
                var ly = layout.oy + (midR + 0.5) * layout.cell;
                var labelColor = (PAL2[grpL.color] && PAL2[grpL.color].belly) || '#ffe9b0';
                var groupScore = scoreChain(grpL.size, chain - 1);
                G.Particles.popLabel(lx, ly - 10, '+' + groupScore, labelColor, false);
            }
            if (chain >= 2) {
                var px = layout.ox + layout.boardW / 2;
                var py = layout.oy + layout.boardH / 2;
                G.Particles.popLabel(px, py, 'CHAIN x' + chain, '#ffd980', true);
            }
        }

        // Timed bonus.
        if (mode === 'timed') {
            modeTimer = Math.min(120000, modeTimer + 150 * count);
        }
    }

    function applyFlashRemoval() {
        // Remove tiles; unlock bonus = count locked tiles that are cleared.
        var unlocksHere = 0;
        for (var i = 0; i < flashTiles.length; i++) {
            var t = flashTiles[i];
            var gref = grid[t.r][t.c];
            if (gref && gref.locked) { unlocksHere++; }
            grid[t.r][t.c] = null;
        }
        if (unlocksHere > 0) stats.unlocks += unlocksHere;

        // Place upgrades.
        if (pendingUpgrades) {
            for (var u = 0; u < pendingUpgrades.length; u++) {
                var up = pendingUpgrades[u];
                // The cell may already hold the "surviving" matched puff; replace
                // it with the specialized version of same color.
                grid[up.r][up.c] = makePuff(up.color, up.special, false);
                if (up.special === SPECIAL_ARROW) grid[up.r][up.c].arrowDir = up.arrowDir || 'h';
            }
            pendingUpgrades = null;
        }
        flashTiles = [];
        flashTimer = 0;
    }

    // Gravity + fill new puffs at the top.
    function collapseAndFill() {
        for (var c = 0; c < cols; c++) {
            var write = rows - 1;
            for (var r = rows - 1; r >= 0; r--) {
                if (grid[r][c]) {
                    if (r !== write) {
                        grid[write][c] = grid[r][c];
                        grid[r][c] = null;
                        var fromY = (r - write) * layout.cell; // negative of actual fall
                        anims.push({ type: 'fall', r: write, c: c, fromOffset: -fromY, t: 0, dur: FALL_MS });
                    }
                    write--;
                }
            }
            for (var w = write; w >= 0; w--) {
                var color = 1 + Math.floor(rnd() * COLORS_N);
                grid[w][c] = makePuff(color, 0, false);
                var fromOff = -(write - w + 2) * layout.cell;
                anims.push({ type: 'fall', r: w, c: c, fromOffset: fromOff, t: 0, dur: FALL_MS });
            }
        }
    }

    // Classic mode: occasionally freeze a puff as a locked one to raise tension.
    function maybeSpawnLock(dt) {
        if (mode !== 'classic') return;
        lockTimer += dt;
        var interval = Math.max(12000, 30000 - level * 1500);
        if (lockTimer >= interval) {
            lockTimer = 0;
            // Find a random non-locked cell that isn't part of a current animation.
            for (var attempt = 0; attempt < 20; attempt++) {
                var r = Math.floor(Math.random() * rows);
                var c = Math.floor(Math.random() * cols);
                if (grid[r][c] && !grid[r][c].locked) {
                    grid[r][c].locked = true;
                    if (G.AppAudio) G.AppAudio.lock();
                    return;
                }
            }
        }
    }

    // ----------------------------------------------------------------------
    // Update loop
    // ----------------------------------------------------------------------

    function isBusy() {
        return snap !== null || flashTiles.length > 0 || anims.length > 0;
    }

    function update(dt) {
        gameTime += dt;

        if (shakeAmp > 0) {
            shakeT -= dt;
            if (shakeT <= 0) { shakeAmp = 0; shakeT = 0; }
        }

        if (mode === 'timed' && !gameOverFlag && !finished) {
            modeTimer -= dt;
            if (modeTimer <= 0) { modeTimer = 0; finished = true; gameOverFlag = true; }
        }

        // Advance snap tween.
        if (snap) {
            snap.t += dt;
            if (snap.t >= snap.dur) {
                applySnap();
            }
        }

        // Flash countdown — and per-tile burst emission as each one pops.
        if (flashTiles.length > 0) {
            flashTimer += dt;
            if (G.Particles) {
                var PALb = G.Puffs.PALETTE;
                for (var fi = 0; fi < flashTiles.length; fi++) {
                    var ft = flashTiles[fi];
                    if (ft.burstFired) continue;
                    if (flashTimer < ft.delay + POP_DUR_MS * 0.35) continue;
                    var gb = grid[ft.r][ft.c];
                    var bcolor = (gb && PALb[gb.color]) ? PALb[gb.color].core : '#ffffff';
                    var bp = cellXY(ft.r, ft.c);
                    G.Particles.burst(bp.x + layout.cell / 2, bp.y + layout.cell / 2,
                                      bcolor, 10 + chain * 2);
                    ft.burstFired = true;
                }
            }
            if (flashTimer >= flashDuration) {
                applyFlashRemoval();
                collapseAndFill();
            }
        }

        // Fall animations.
        var done = [];
        for (var k = 0; k < anims.length; k++) {
            var an = anims[k];
            if (an.type === 'fall') {
                an.t += dt;
                if (an.t >= an.dur) done.push(k);
            }
        }
        for (var d = done.length - 1; d >= 0; d--) anims.splice(done[d], 1);

        // After things settle, hold briefly so each cascade reads as its own
        // beat, then check for new matches.
        if (!isBusy() && !drag && !snap) {
            if (chain > 0 && settleTimer < SETTLE_MS) {
                settleTimer += dt;
                return;
            }
            var groups = findMatches(grid);
            if (groups.length > 0) {
                resolveGroups(groups);
            } else {
                // Chain finished.
                if (chain > 0) chain = 0;
                settleTimer = 0;
                // Mode-specific end checks.
                if (mode === 'classic') {
                    var threshold = level * 15;
                    if (popped >= threshold) {
                        level++;
                        if (G.AppAudio) G.AppAudio.levelUp();
                    }
                    if (!hasAnyMatchingShift(grid)) {
                        // Deadlock = game over in classic.
                        gameOverFlag = true;
                    }
                } else if (mode === 'puzzle') {
                    if (popped >= puzzleTarget) {
                        if (!nextPuzzle()) { /* finished all puzzles */ }
                    } else if (puzzleMovesLeft <= 0) {
                        gameOverFlag = true;
                    }
                }
                maybeSpawnLock(dt);
            }
        }
    }

    // ----------------------------------------------------------------------
    // HUD
    // ----------------------------------------------------------------------

    function updateHUD() {
        var el;
        el = document.getElementById('hud-score'); if (el) el.textContent = String(score);
        el = document.getElementById('hud-level'); if (el) el.textContent = String(level);
        var extraLabel = document.getElementById('hud-extra-label');
        var extraVal = document.getElementById('hud-extra');
        if (mode === 'classic') {
            if (extraLabel) extraLabel.textContent = 'POPPED';
            if (extraVal) extraVal.textContent = String(popped);
        } else if (mode === 'timed') {
            if (extraLabel) extraLabel.textContent = 'TIME';
            if (extraVal) extraVal.textContent = formatTime(modeTimer);
        } else if (mode === 'puzzle') {
            if (extraLabel) extraLabel.textContent = 'MOVES';
            if (extraVal) extraVal.textContent = String(puzzleMovesLeft);
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

    // ----------------------------------------------------------------------
    // Public API
    // ----------------------------------------------------------------------

    return {
        ROWS: ROWS, COLS: COLS, COLORS_N: COLORS_N,
        SPECIAL_NONE: SPECIAL_NONE, SPECIAL_JUMBO: SPECIAL_JUMBO,
        SPECIAL_ARROW: SPECIAL_ARROW, SPECIAL_PRISM: SPECIAL_PRISM,

        // lifecycle
        startGame: startGame,
        update: update,
        calcLayout: calcLayout,
        drawBackground: drawBackground,
        drawBoard: drawBoard,
        updateHUD: updateHUD,
        formatTime: formatTime,

        // input
        handleMouseDown: handleMouseDown,
        handleMouseMove: handleMouseMove,
        handleMouseUp: handleMouseUp,
        cursorMove: cursorMove,
        cursorAction: cursorAction,
        cursorSlide: cursorSlide,

        // pure helpers / test hooks
        makeEmptyGrid: makeEmptyGrid,
        makePuff: makePuff,
        slideRow: slideRow,
        slideCol: slideCol,
        findMatches: findMatches,
        legalShifts: legalShifts,
        hasAnyMatchingShift: hasAnyMatchingShift,
        seedGrid: seedGrid,
        scoreChain: scoreChain,
        copyGrid: copyGrid,

        // introspection
        getGrid: function () { return grid; },
        setGrid: function (g) { grid = g; rows = g.length; cols = g[0] ? g[0].length : 0; },
        getScore: function () { return score; },
        setScore: function (v) { score = v; },
        addScore: function (v) { score += v; },
        getPopped: function () { return popped; },
        getMoves: function () { return moves; },
        getLevel: function () { return level; },
        getMode: function () { return mode; },
        getChain: function () { return chain; },
        getMaxChain: function () { return maxChain; },
        getStats: function () { return stats; },
        getModeTimer: function () { return modeTimer; },
        getPuzzleIndex: function () { return puzzleIndex; },
        getPuzzleMovesLeft: function () { return puzzleMovesLeft; },
        isAnimating: function () { return isBusy(); },
        isGameOver: function () { return gameOverFlag; },
        isFinished: function () { return finished; },
        getCursor: function () { return cursor; },
        getDrag: function () { return drag; },
        getLayout: function () { return layout; },
        cellXY: cellXY,
        pointToCell: pointToCell,

        // For tests: force-resolve the board (apply one chain of matches).
        resolveMatchesNow: function () {
            chain = 0;
            resolveMatches();
        },

        setSeed: setSeed,
    };
})();
