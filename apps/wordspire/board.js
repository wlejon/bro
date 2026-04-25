// board.js — letter-grid word-builder. Grid, chain, pop, settle, burning tiles.
'use strict';
var W = window.W = window.W || {};

W.Board = (function () {
    // ---- Constants ------------------------------------------------------
    var COLS = 7;
    var ROWS = 8;

    // Mult levels for special tiles.
    var MULT_NORMAL   = 1;
    var MULT_GILDED   = 2;   // gold
    var MULT_JEWELED  = 3;   // emerald
    var MULT_SAPPHIRE = 4;
    var MULT_RUBY     = 5;

    // English-ish letter frequency (rough Scrabble distribution).
    var LETTER_FREQ = [
        ['a', 9], ['b', 2], ['c', 3], ['d', 4], ['e', 13], ['f', 2],
        ['g', 3], ['h', 4], ['i', 9], ['j', 1], ['k', 1], ['l', 4],
        ['m', 3], ['n', 7], ['o', 8], ['p', 2], ['q', 1], ['r', 7],
        ['s', 6], ['t', 8], ['u', 4], ['v', 2], ['w', 2], ['x', 1],
        ['y', 3], ['z', 1]
    ];
    // Ensure we always have enough vowels — append extras if short.
    var LETTER_POOL = (function () {
        var a = [];
        for (var i = 0; i < LETTER_FREQ.length; i++) {
            for (var j = 0; j < LETTER_FREQ[i][1]; j++) a.push(LETTER_FREQ[i][0]);
        }
        return a;
    })();

    // ---- RNG ------------------------------------------------------------
    var seed = 1;
    function rnd() {
        // xorshift32
        seed |= 0;
        seed ^= (seed << 13);
        seed ^= (seed >>> 17);
        seed ^= (seed << 5);
        return ((seed >>> 0) / 4294967296);
    }
    function setSeed(s) { seed = s | 0 || 1; }

    function pickLetter() {
        return LETTER_POOL[Math.floor(rnd() * LETTER_POOL.length)];
    }

    // ---- State ----------------------------------------------------------
    // grid[col][row]. row 0 is the TOP of the column, row ROWS-1 is the BOTTOM.
    // Each cell is a Tile or null.
    // Tile: { letter, mult, burning, id }
    var grid = null;
    var tileIdCounter = 1;

    var currentChain = [];       // array of [col, row]
    var currentPath  = [];       // same as chain, cached for fast validity
    var cursorCol = 3, cursorRow = 4;

    var mode = 'classic';
    var score = 0;
    var level = 1;
    var streak = 0;
    var wordsPlayed = 0;
    var longestWord = '';
    var bestWordScore = 0;
    var bestWordText  = '';
    var gameOver = false;
    var finished = false;
    var gameTime = 0;
    var timedRemaining = 0;
    var puzzleIndex = 0;
    var puzzleTarget = '';
    var puzzleSolved = 0;
    var puzzleTotal  = 20;

    // Pending pops: cells that were popped this submission but haven't been
    // re-filled yet. Used for animation.
    var popAnim = [];   // { col, row, letter, timer, life }

    // Animation state for a single settle step.
    var animTime = 0;

    // ---- Grid helpers --------------------------------------------------
    function makeEmptyGrid() {
        var g = new Array(COLS);
        for (var c = 0; c < COLS; c++) {
            g[c] = new Array(ROWS);
            for (var r = 0; r < ROWS; r++) g[c][r] = null;
        }
        return g;
    }

    function newTile(letter, mult, burning) {
        return {
            letter: letter || pickLetter(),
            mult: mult || 1,
            burning: !!burning,
            id: tileIdCounter++
        };
    }

    function fillGrid(g) {
        for (var c = 0; c < COLS; c++) {
            for (var r = 0; r < ROWS; r++) {
                if (!g[c][r]) g[c][r] = newTile();
            }
        }
    }

    function cloneGrid(g) {
        var out = new Array(COLS);
        for (var c = 0; c < COLS; c++) {
            out[c] = g[c].slice();
        }
        return out;
    }

    // ---- Path validation -----------------------------------------------
    // isValidPath(path, grid): true iff every consecutive pair in `path`
    // is adjacent (8-neighbors), no cell repeats, and all cells are occupied.
    function isValidPath(path, g) {
        if (!path || path.length === 0) return false;
        var seen = {};
        for (var i = 0; i < path.length; i++) {
            var c = path[i][0], r = path[i][1];
            if (c < 0 || c >= COLS || r < 0 || r >= ROWS) return false;
            if (g && !g[c][r]) return false;
            var key = c + ',' + r;
            if (seen[key]) return false;
            seen[key] = true;
            if (i > 0) {
                var pc = path[i - 1][0], pr = path[i - 1][1];
                var dc = Math.abs(c - pc), dr = Math.abs(r - pr);
                if (dc > 1 || dr > 1 || (dc === 0 && dr === 0)) return false;
            }
        }
        return true;
    }

    function pathToWord(path, g) {
        var s = '';
        for (var i = 0; i < path.length; i++) {
            var t = g[path[i][0]][path[i][1]];
            if (!t) return '';
            s += t.letter;
        }
        return s.toLowerCase();
    }

    function pathToTiles(path, g) {
        var out = [];
        for (var i = 0; i < path.length; i++) {
            out.push(g[path[i][0]][path[i][1]]);
        }
        return out;
    }

    // ---- Settle (gravity) ----------------------------------------------
    // After popping, each column "falls": non-null tiles stack at the bottom,
    // nulls at the top. Then new tiles spawn at the top.
    function settle(g, opts) {
        opts = opts || {};
        for (var c = 0; c < COLS; c++) {
            var col = g[c];
            // Gather non-null tiles preserving order.
            var stack = [];
            for (var r = 0; r < ROWS; r++) {
                if (col[r]) stack.push(col[r]);
            }
            // Re-stack at the bottom.
            var newCol = new Array(ROWS);
            for (var i = 0; i < ROWS; i++) newCol[i] = null;
            var bottom = ROWS - 1;
            for (var j = stack.length - 1; j >= 0; j--) {
                newCol[bottom--] = stack[j];
            }
            g[c] = newCol;
        }
        if (!opts.skipFill) fillGrid(g);
    }

    // ---- Burning tiles --------------------------------------------------
    // Advance burning tiles down by one row where possible. If a burning
    // tile is already at the bottom, this triggers game over.
    function descendBurning(g) {
        var collapsed = false;
        // Walk columns from bottom to top so tiles don't leapfrog.
        for (var c = 0; c < COLS; c++) {
            for (var r = ROWS - 1; r >= 0; r--) {
                var t = g[c][r];
                if (!t || !t.burning) continue;
                if (r === ROWS - 1) {
                    // Already at bottom: collapse.
                    collapsed = true;
                    continue;
                }
                // Swap downward with the tile below (if burning tile is already
                // in that slot, it will get handled next pass).
                var below = g[c][r + 1];
                if (below && !below.burning) {
                    // Shift this burning tile down. The tile below moves up.
                    g[c][r + 1] = t;
                    g[c][r] = below;
                    t._descended = true;
                }
            }
        }
        // Clear per-cycle flags.
        for (var cc = 0; cc < COLS; cc++) {
            for (var rr = 0; rr < ROWS; rr++) {
                if (g[cc][rr]) g[cc][rr]._descended = false;
            }
        }
        return collapsed;
    }

    // Spawn a burning tile at the top of a random non-full column.
    function sprinkleBurning(g, countHint) {
        var candidates = [];
        for (var c = 0; c < COLS; c++) {
            // Fine-grained: any column with a top cell is valid.
            if (g[c][0]) candidates.push(c);
        }
        if (candidates.length === 0) return 0;
        var count = countHint || 1;
        var placed = 0;
        for (var i = 0; i < count; i++) {
            var c2 = candidates[Math.floor(rnd() * candidates.length)];
            if (!g[c2][0].burning) {
                g[c2][0].burning = true;
                placed++;
            }
        }
        return placed;
    }

    // ---- Reward tile placement (after a pop) ---------------------------
    // When a word of length L pops, spawn one special tile of appropriate
    // tier in one of the freshly-filled positions at the top.
    function dropReward(g, length) {
        var mult = 0;
        if (length >= 8)      mult = MULT_RUBY;
        else if (length >= 7) mult = MULT_SAPPHIRE;
        else if (length >= 6) mult = MULT_JEWELED;
        else if (length >= 5) mult = MULT_GILDED;
        if (!mult) return;

        // Put the mult on a random top-row tile.
        var c = Math.floor(rnd() * COLS);
        var t = g[c][0];
        if (t) t.mult = mult;
    }

    // ---- Pop cells ------------------------------------------------------
    // Remove all cells in `path` from the grid (set to null), queue pop
    // animations, return the number popped.
    function popCells(path, g) {
        var n = 0;
        for (var i = 0; i < path.length; i++) {
            var c = path[i][0], r = path[i][1];
            var t = g[c][r];
            if (!t) continue;
            popAnim.push({
                col: c, row: r,
                letter: t.letter, mult: t.mult, burning: t.burning,
                life: 380, timer: 380
            });
            g[c][r] = null;
            n++;
        }
        return n;
    }

    // ---- findMatches ----------------------------------------------------
    // Walk the grid and find all possible 3+ letter paths that form valid
    // dictionary words. Primarily used for hints/puzzle gen — expensive.
    function findMatches(maxResults) {
        maxResults = maxResults || 20;
        if (!W.Dictionary.loaded()) return [];
        var found = [];
        var seenWords = {};
        var g = grid;
        function dfs(path, word) {
            if (found.length >= maxResults) return;
            if (word.length >= 3 && W.Dictionary.isWord(word) && !seenWords[word]) {
                seenWords[word] = true;
                found.push({ word: word, path: path.slice() });
            }
            if (word.length >= 9) return;
            if (word.length >= 2 && !W.Dictionary.isPrefix(word)) return;
            var last = path[path.length - 1];
            for (var dc = -1; dc <= 1; dc++) {
                for (var dr = -1; dr <= 1; dr++) {
                    if (dc === 0 && dr === 0) continue;
                    var nc = last[0] + dc, nr = last[1] + dr;
                    if (nc < 0 || nc >= COLS || nr < 0 || nr >= ROWS) continue;
                    var t = g[nc][nr];
                    if (!t) continue;
                    var key = nc + ',' + nr;
                    var dup = false;
                    for (var k = 0; k < path.length; k++) {
                        if (path[k][0] === nc && path[k][1] === nr) { dup = true; break; }
                    }
                    if (dup) continue;
                    path.push([nc, nr]);
                    dfs(path, word + t.letter);
                    path.pop();
                    if (found.length >= maxResults) return;
                }
            }
        }
        for (var c = 0; c < COLS; c++) {
            for (var r = 0; r < ROWS; r++) {
                var t = g[c][r];
                if (!t) continue;
                dfs([[c, r]], t.letter);
                if (found.length >= maxResults) return found;
            }
        }
        return found;
    }

    // ---- Submit chain ---------------------------------------------------
    function submitChain() {
        if (currentChain.length < 3) {
            W.Audio.submitFail();
            W.Particles.showAction('TOO SHORT');
            resetChain();
            return false;
        }
        if (!isValidPath(currentChain, grid)) {
            W.Audio.submitFail();
            resetChain();
            return false;
        }
        var word = pathToWord(currentChain, grid);
        if (!W.Dictionary.isWord(word)) {
            W.Audio.submitFail();
            W.Particles.showAction('NOT A WORD');
            streak = 0;
            updateCombo();
            resetChain();
            return false;
        }

        // Valid word!
        var tiles = pathToTiles(currentChain, grid);
        var base  = W.Scoring.computeWordScore(word, tiles);
        streak++;
        var comboMult = W.Scoring.comboMultiplier(streak);
        var pts = Math.floor(base * comboMult);

        // Burning tiles in the chain are extinguished.
        var burnedOut = 0;
        for (var i = 0; i < tiles.length; i++) {
            if (tiles[i] && tiles[i].burning) burnedOut++;
        }

        // Particle + visual.
        spawnPopBursts(currentChain, word.length);
        popCells(currentChain, grid);
        settle(grid, { skipFill: true });
        // Reward tiles drop in the new top row.
        // Fill first so we have tiles to decorate.
        fillGrid(grid);
        dropReward(grid, word.length);

        // Burning tile mechanic — classic mode only.
        if (mode === 'classic') {
            // Descend existing burning tiles by one row each turn.
            var collapsed = descendBurning(grid);
            if (collapsed) {
                triggerGameOver();
                return true;
            }
            // Spawn a new burning tile every few words based on difficulty.
            var burnChance = burnChancePerWord();
            if (rnd() < burnChance) {
                var howMany = 1 + (wordsPlayed > 12 ? 1 : 0);
                sprinkleBurning(grid, Math.min(howMany, 2));
                W.Audio.sizzle();
            }
        }

        // Stats.
        score += pts;
        wordsPlayed++;
        if (word.length > longestWord.length) longestWord = word;
        if (pts > bestWordScore) { bestWordScore = pts; bestWordText = word; }

        // Top-word persistence.
        try {
            W.Storage.addWord({
                word: word,
                score: pts,
                length: word.length,
                mode: mode,
                date: (new Date()).toISOString().slice(0, 10)
            });
        } catch (e) {}

        // Audio/visual feedback.
        W.Audio.submitOk(word.length);
        if (word.length >= 7) {
            W.Audio.fanfare();
            W.Particles.showAction(word.toUpperCase() + '!  +' + pts);
        } else {
            W.Particles.showAction(word.toUpperCase() + '  +' + pts);
        }

        // Level up every 10 words.
        var newLevel = 1 + Math.floor(wordsPlayed / 10);
        if (newLevel > level) level = newLevel;

        // Puzzle mode: check target.
        if (mode === 'puzzle' && word === puzzleTarget) {
            puzzleSolved++;
            if (puzzleSolved >= puzzleTotal) {
                finished = true;
            } else {
                startPuzzle(puzzleIndex + 1);
            }
        }

        updateCombo();
        updateHUD();
        resetChain();
        return true;
    }

    function burnChancePerWord() {
        // 0 = easy, 1 = normal, 2 = hard
        var d = W.Storage.settings.difficulty | 0;
        if (d === 0) return 0.18;
        if (d === 2) return 0.55;
        return 0.32;
    }

    function spawnPopBursts(path, length) {
        var rect = boardRect(lastW, lastH);
        for (var i = 0; i < path.length; i++) {
            var c = path[i][0], r = path[i][1];
            var cx = rect.x + c * rect.cell + rect.cell * 0.5;
            var cy = rect.y + r * rect.cell + rect.cell * 0.5;
            var color = length >= 7 ? '#e8c168' : (length >= 5 ? '#8cdff6' : '#c8b8e8');
            W.Particles.burst(cx, cy, color);
        }
        if (length >= 6) W.Particles.shake(300, 6);
    }

    // ---- Chain building -------------------------------------------------
    function resetChain() {
        currentChain = [];
        currentPath  = [];
    }

    function tryAddTile(c, r) {
        if (c < 0 || c >= COLS || r < 0 || r >= ROWS) return false;
        if (!grid[c][r]) return false;
        if (currentChain.length === 0) {
            currentChain.push([c, r]);
            W.Audio.tileAdd(1);
            return true;
        }
        var last = currentChain[currentChain.length - 1];
        if (last[0] === c && last[1] === r) {
            // Same tile = treat as "remove last".
            removeLastTile();
            return false;
        }
        // Backtrack if tapping the second-to-last tile.
        if (currentChain.length >= 2) {
            var prev = currentChain[currentChain.length - 2];
            if (prev[0] === c && prev[1] === r) {
                removeLastTile();
                return true;
            }
        }
        // Must be adjacent and not already in chain.
        var dc = Math.abs(c - last[0]), dr = Math.abs(r - last[1]);
        if (dc > 1 || dr > 1 || (dc === 0 && dr === 0)) return false;
        for (var i = 0; i < currentChain.length; i++) {
            if (currentChain[i][0] === c && currentChain[i][1] === r) return false;
        }
        currentChain.push([c, r]);
        W.Audio.tileAdd(currentChain.length);
        return true;
    }

    function removeLastTile() {
        if (currentChain.length === 0) return;
        currentChain.pop();
        W.Audio.tileRemove();
    }

    function clearChain() {
        if (currentChain.length === 0) return;
        currentChain = [];
        W.Audio.clearChain();
    }

    function updateCombo() {
        var el = document.getElementById('hud-combo-label');
        var val = document.getElementById('hud-combo');
        if (streak >= 2) {
            if (el) el.style.display = 'block';
            if (val) { val.style.display = 'block'; val.textContent = 'x' + streak; }
        } else {
            if (el) el.style.display = 'none';
            if (val) val.style.display = 'none';
        }
    }

    // ---- Game lifecycle -------------------------------------------------
    function startGame(m) {
        mode = m || 'classic';
        score = 0;
        level = 1;
        streak = 0;
        wordsPlayed = 0;
        longestWord = '';
        bestWordScore = 0;
        bestWordText = '';
        gameOver = false;
        finished = false;
        gameTime = 0;
        timedRemaining = (mode === 'timed') ? 180 * 1000 : 0;
        setSeed((Date.now() & 0xffffffff) || 1);
        grid = makeEmptyGrid();
        fillGrid(grid);
        // Ensure at least one valid word exists initially — retry up to 10x.
        for (var k = 0; k < 10; k++) {
            if (W.Dictionary.loaded()) {
                var hints = findMatches(1);
                if (hints.length > 0) break;
            }
            grid = makeEmptyGrid();
            fillGrid(grid);
        }
        popAnim = [];
        currentChain = [];
        cursorCol = Math.floor(COLS / 2);
        cursorRow = Math.floor(ROWS / 2);
        W.Particles.clear();

        if (mode === 'puzzle') {
            puzzleIndex = 0;
            puzzleSolved = 0;
            startPuzzle(0);
        }

        updateHUD();
        updateCombo();
    }

    function startPuzzle(idx) {
        puzzleIndex = idx;
        // Pick a target word from dictionary that's 6-7 letters long.
        if (!W.Dictionary.loaded()) {
            puzzleTarget = '';
            return;
        }
        // Walk dictionary collecting candidates; deterministic via seed.
        setSeed(0x1234 + idx * 7919);
        // Dump grid, fill with random letters — need to guarantee the target
        // is constructible. Simpler approach: generate random grid until
        // findMatches yields at least one 5+ letter word, use that as target.
        for (var attempt = 0; attempt < 30; attempt++) {
            grid = makeEmptyGrid();
            fillGrid(grid);
            var hits = findMatches(12);
            // Prefer length >= 5.
            var best = null;
            for (var i = 0; i < hits.length; i++) {
                if (hits[i].word.length >= 5) {
                    if (!best || hits[i].word.length > best.word.length) best = hits[i];
                }
            }
            if (best) { puzzleTarget = best.word; break; }
            if (hits.length) { puzzleTarget = hits[0].word; break; }
        }
        updateHUD();
    }

    function triggerGameOver() {
        gameOver = true;
        W.Audio.gameover();
        W.Particles.shake(500, 12);
    }

    // ---- HUD ------------------------------------------------------------
    function updateHUD() {
        Hud.text('#hud-score', String(score));
        Hud.text('#hud-level', String(level));
        var label = document.getElementById('hud-extra-label');
        if (mode === 'timed') {
            if (label) label.textContent = 'TIME';
            var sec = Math.max(0, Math.ceil(timedRemaining / 1000));
            var m = Math.floor(sec / 60);
            var s = sec % 60;
            Hud.text('#hud-extra', m + ':' + (s < 10 ? '0' : '') + s);
        } else if (mode === 'puzzle') {
            if (label) label.textContent = 'PUZZLE';
            Hud.text('#hud-extra', (puzzleSolved + 1) + '/' + puzzleTotal);
        } else {
            if (label) label.textContent = 'WORDS';
            Hud.text('#hud-extra', String(wordsPlayed));
        }
        Hud.text('#hud-longest', longestWord ? longestWord.toUpperCase() : '-');
        Hud.text('#hud-best', bestWordText ? (bestWordText.toUpperCase() + ' (' + bestWordScore + ')') : '-');

        // Burning warning (classic): any burning tile in bottom 2 rows?
        var warn = false;
        if (mode === 'classic' && grid) {
            for (var c = 0; c < COLS; c++) {
                for (var r = ROWS - 2; r < ROWS; r++) {
                    if (grid[c][r] && grid[c][r].burning) { warn = true; break; }
                }
                if (warn) break;
            }
        }
        var w = document.getElementById('burning-warn');
        if (w) {
            w.textContent = '! BURNING TILE DANGER !';
            w.style.display = warn ? 'block' : 'none';
        }
    }

    // ---- Tick / draw ---------------------------------------------------
    var lastW = 1000, lastH = 800;
    function tick(dt) {
        gameTime += dt;
        animTime += dt;
        W.Particles.update(dt);
        // Pop animation counters.
        for (var i = popAnim.length - 1; i >= 0; i--) {
            popAnim[i].timer -= dt;
            if (popAnim[i].timer <= 0) popAnim.splice(i, 1);
        }
        if (mode === 'timed') {
            timedRemaining -= dt;
            if (timedRemaining <= 0) {
                timedRemaining = 0;
                finished = true;
            }
        }
        // Refresh HUD time every frame in timed mode.
        if (mode === 'timed') updateHUD();
    }

    function boardRect(W_, H_) {
        var margin = 60;
        var availW = W_ - margin * 2 - 160; // leave room for HUD on left
        var availH = H_ - margin * 2;
        var cell = Math.floor(Math.min(availW / COLS, availH / ROWS));
        if (cell < 32) cell = 32;
        var gridW = cell * COLS;
        var gridH = cell * ROWS;
        var x = Math.floor((W_ - gridW) / 2 + 60); // bias right of HUD
        var y = Math.floor((H_ - gridH) / 2);
        return { x: x, y: y, cell: cell, cols: COLS, rows: ROWS };
    }

    function roundRect(ctx, x, y, w, h, r) {
        ctx.beginPath();
        ctx.moveTo(x + r, y);
        ctx.arcTo(x + w, y, x + w, y + h, r);
        ctx.arcTo(x + w, y + h, x, y + h, r);
        ctx.arcTo(x, y + h, x, y, r);
        ctx.arcTo(x, y, x + w, y, r);
        ctx.closePath();
    }

    function showTileLayer(show) {
        // No-op now that tiles render on canvas via pixel font. Kept for API.
    }

    function drawTile(ctx, tile, x, y, cell, state, time) {
        var pad = 4;
        var w = cell - pad * 2, h = cell - pad * 2;
        var rx = x + pad, ry = y + pad;

        var bg = '#25203a';
        var border = '#3a3258';
        var text = '#f0e0ff';
        if (tile.mult === 2) { bg = '#504020'; border = '#c6a240'; text = '#ffecb0'; }
        else if (tile.mult === 3) { bg = '#1f4432'; border = '#3fd596'; text = '#b8ffdc'; }
        else if (tile.mult === 4) { bg = '#1d2f58'; border = '#4fa8ff'; text = '#c8e0ff'; }
        else if (tile.mult === 5) { bg = '#4a1828'; border = '#ff6488'; text = '#ffc8d8'; }
        if (tile.burning) { bg = '#4a1810'; border = '#ff4b3d'; text = '#ffbcb0'; }
        if (state === 'chain')       border = '#e8c168';
        else if (state === 'cursor') border = '#8cdff6';

        roundRect(ctx, rx, ry, w, h, 8);
        ctx.fillStyle = bg;
        ctx.fill();
        if (tile.burning) {
            var gr = ctx.createLinearGradient(rx, ry + h, rx, ry);
            gr.addColorStop(0, 'rgba(255,75,61,0.7)');
            gr.addColorStop(1, 'rgba(255,170,60,0.25)');
            ctx.fillStyle = gr;
            ctx.fill();
        }
        ctx.lineWidth = state ? 3 : 2;
        ctx.strokeStyle = border;
        ctx.stroke();

        // Inner bevel.
        ctx.save();
        ctx.strokeStyle = 'rgba(255,255,255,0.08)';
        ctx.lineWidth = 1;
        roundRect(ctx, rx + 2, ry + 2, w - 4, h - 4, 6);
        ctx.stroke();
        ctx.restore();

        // Pixel-font letter (centered).
        var letterScale = Math.max(2, Math.floor(cell / 12));
        W.Text.drawCentered(ctx, tile.letter,
                                 rx + w / 2, ry + h / 2 - letterScale,
                                 letterScale, text);

        // Letter-value chip bottom-right.
        var v = W.Scoring.letterValue(tile.letter);
        var vScale = Math.max(1, Math.floor(cell / 28));
        var vw = W.Text.measure(String(v), vScale);
        W.Text.draw(ctx, String(v),
                         rx + w - vw - 4, ry + h - 7 * vScale - 3,
                         vScale, 'rgba(220,220,255,0.7)');

        // Multiplier chip top-left (x2/x3/...).
        if (tile.mult > 1) {
            var mScale = Math.max(1, Math.floor(cell / 28));
            W.Text.draw(ctx, 'x' + tile.mult, rx + 4, ry + 3, mScale, border);
        }
    }

    function drawBoard(ctx, W_, H_, time) {
        lastW = W_; lastH = H_;
        if (!grid) return;
        var r = boardRect(W_, H_);

        // Spire backdrop.
        ctx.save();
        ctx.fillStyle = 'rgba(40,30,60,0.35)';
        roundRect(ctx, r.x - 14, r.y - 14, r.cell * COLS + 28, r.cell * ROWS + 28, 14);
        ctx.fill();
        ctx.strokeStyle = 'rgba(160,130,220,0.2)';
        ctx.lineWidth = 1;
        ctx.stroke();
        ctx.restore();

        // Tiles.
        var chainKeys = {};
        for (var ci = 0; ci < currentChain.length; ci++) {
            chainKeys[currentChain[ci][0] + ',' + currentChain[ci][1]] = ci;
        }
        for (var c = 0; c < COLS; c++) {
            for (var row = 0; row < ROWS; row++) {
                var t = grid[c][row];
                if (!t) continue;
                var x = r.x + c * r.cell;
                var y = r.y + row * r.cell;
                var key = c + ',' + row;
                var state = null;
                if (chainKeys[key] != null) state = 'chain';
                else if (c === cursorCol && row === cursorRow) state = 'cursor';
                drawTile(ctx, t, x, y, r.cell, state, time);
            }
        }

        // Chain path overlay.
        if (currentChain.length >= 2) {
            ctx.save();
            ctx.strokeStyle = 'rgba(232,193,104,0.85)';
            ctx.lineWidth = Math.max(3, r.cell * 0.08);
            ctx.lineCap = 'round';
            ctx.beginPath();
            for (var p = 0; p < currentChain.length; p++) {
                var cx = r.x + currentChain[p][0] * r.cell + r.cell / 2;
                var cy = r.y + currentChain[p][1] * r.cell + r.cell / 2;
                if (p === 0) ctx.moveTo(cx, cy);
                else ctx.lineTo(cx, cy);
            }
            ctx.stroke();
            ctx.restore();
        }

        // Pop animations.
        for (var pi = 0; pi < popAnim.length; pi++) {
            var pa = popAnim[pi];
            var px = r.x + pa.col * r.cell + r.cell / 2;
            var py = r.y + pa.row * r.cell + r.cell / 2;
            var a = pa.timer / pa.life;
            ctx.save();
            ctx.globalAlpha = a;
            var ls = Math.max(2, Math.floor(r.cell * (0.08 + (1 - a) * 0.06)));
            W.Text.drawCentered(ctx, pa.letter, px, py, ls, '#e8c168');
            ctx.restore();
        }

        // Particles.
        W.Particles.draw(ctx);

        // Word preview below board.
        var previewY = r.y + r.cell * ROWS + 18;
        var word = pathToWord(currentChain, grid);
        if (word) {
            var previewScale = 4;
            W.Text.drawCentered(ctx, word.toUpperCase(),
                                     r.x + r.cell * COLS / 2, previewY,
                                     previewScale, '#e0d4ff');
            var predicted = W.Scoring.computeWordScore(word, pathToTiles(currentChain, grid));
            var valid = W.Dictionary.isWord(word);
            W.Text.drawCentered(ctx, '+' + predicted + (valid ? '' : ' ?'),
                                     r.x + r.cell * COLS / 2, previewY + 36,
                                     2, valid ? '#8cdff6' : '#aa7788');
        } else {
            var hint = (mode === 'puzzle' && puzzleTarget)
                ? ('TARGET: ' + puzzleTarget.toUpperCase())
                : 'CHAIN 3+ LETTERS AND SUBMIT';
            W.Text.drawCentered(ctx, hint,
                                     r.x + r.cell * COLS / 2, previewY + 8,
                                     2, '#554966');
        }

        // Submit + clear buttons.
        drawSubmitButton(ctx, r);

        // Spire side illustration.
        drawSpire(ctx, W_, H_);
    }

    function drawSubmitButton(ctx, r) {
        var bx = r.x + r.cell * COLS + 20;
        var by = r.y;
        var bw = 120, bh = 44;
        if (bx + bw > lastW - 8) return;
        roundRect(ctx, bx, by, bw, bh, 8);
        ctx.fillStyle = currentChain.length >= 3 ? 'rgba(232,193,104,0.3)' : 'rgba(50,40,70,0.6)';
        ctx.fill();
        ctx.strokeStyle = currentChain.length >= 3 ? '#e8c168' : '#3a3258';
        ctx.lineWidth = 2;
        ctx.stroke();
        W.Text.drawCentered(ctx, 'SUBMIT', bx + bw / 2, by + bh / 2, 3,
                                 currentChain.length >= 3 ? '#fff4d8' : '#7a6a9a');

        var cx2 = bx, cy2 = by + bh + 10;
        roundRect(ctx, cx2, cy2, bw, 34, 6);
        ctx.fillStyle = 'rgba(50,40,70,0.6)';
        ctx.fill();
        ctx.strokeStyle = '#3a3258';
        ctx.stroke();
        W.Text.drawCentered(ctx, 'CLEAR', cx2 + bw / 2, cy2 + 17, 2, '#b8a8d8');
    }


    function drawSpire(ctx, W_, H_) {
        // Tower of letters building on the right side.
        var sx = W_ - 40;
        var sy = H_ - 40;
        var levels = Math.min(40, wordsPlayed);
        for (var i = 0; i < levels; i++) {
            var bw = 18 - Math.floor(i / 8) * 2;
            var bh = 8;
            var bx = sx - bw / 2;
            var by = sy - (i + 1) * (bh + 2);
            ctx.fillStyle = i % 5 === 4 ? '#e8c168' : (i % 3 === 0 ? '#8cdff6' : '#c8b8e8');
            ctx.fillRect(bx, by, bw, bh);
            ctx.strokeStyle = 'rgba(0,0,0,0.35)';
            ctx.lineWidth = 1;
            ctx.strokeRect(bx, by, bw, bh);
        }
    }

    // ---- Mouse ----------------------------------------------------------
    function mouseClick(x, y) {
        var r = boardRect(lastW, lastH);
        // Submit button?
        var bx = r.x + r.cell * COLS + 20;
        var by = r.y;
        if (x >= bx && x <= bx + 120 && y >= by && y <= by + 44) {
            submitChain();
            return;
        }
        // Clear button below submit
        if (x >= bx && x <= bx + 120 && y >= by + 54 && y <= by + 88) {
            clearChain();
            return;
        }
        // Board tile?
        if (x < r.x || x >= r.x + r.cell * COLS) return;
        if (y < r.y || y >= r.y + r.cell * ROWS) return;
        var c = Math.floor((x - r.x) / r.cell);
        var row = Math.floor((y - r.y) / r.cell);
        if (c < 0 || c >= COLS || row < 0 || row >= ROWS) return;
        cursorCol = c; cursorRow = row;
        tryAddTile(c, row);
    }

    function mouseDblClick(x, y) {
        submitChain();
    }

    // ---- Keyboard -------------------------------------------------------
    function moveCursor(dc, dr) {
        cursorCol = Math.max(0, Math.min(COLS - 1, cursorCol + dc));
        cursorRow = Math.max(0, Math.min(ROWS - 1, cursorRow + dr));
    }

    function keyAddAtCursor() {
        tryAddTile(cursorCol, cursorRow);
    }

    // ---- Test helpers ---------------------------------------------------
    function setGrid(letters) {
        // letters: array of ROWS strings of length COLS (row 0 at top).
        grid = makeEmptyGrid();
        for (var rr = 0; rr < ROWS && rr < letters.length; rr++) {
            var row = letters[rr];
            for (var cc = 0; cc < COLS && cc < row.length; cc++) {
                var ch = row.charAt(cc);
                if (ch && ch !== ' ') {
                    grid[cc][rr] = newTile(ch.toLowerCase(), 1, false);
                }
            }
        }
        // Fill any holes after a seeded layout.
        fillGrid(grid);
        popAnim = [];
    }

    function setChain(path) {
        currentChain = path.slice();
    }

    function playWordByPath(path) {
        setChain(path);
        return submitChain();
    }

    function forceBurnAt(c, r) {
        if (grid[c][r]) grid[c][r].burning = true;
    }

    return {
        // Constants
        COLS: COLS, ROWS: ROWS,
        MULT_GILDED: MULT_GILDED, MULT_JEWELED: MULT_JEWELED,
        MULT_SAPPHIRE: MULT_SAPPHIRE, MULT_RUBY: MULT_RUBY,

        // Lifecycle
        startGame: startGame,
        tick: tick,
        draw: drawBoard,
        isGameOver: function () { return gameOver; },
        isFinished: function () { return finished; },
        getMode: function () { return mode; },

        // State accessors
        getGrid: function () { return grid; },
        setGridTest: setGrid,
        getChain: function () { return currentChain.slice(); },
        setChainTest: setChain,
        getScore: function () { return score; },
        setScore: function (s) { score = s; },
        getStats: function () {
            return {
                mode: mode, score: score, level: level, words: wordsPlayed,
                longest: longestWord, bestWord: bestWordText, bestWordScore: bestWordScore,
                finished: finished, gameTime: gameTime
            };
        },

        // Input (mouse/keyboard)
        mouseClick: mouseClick,
        mouseDblClick: mouseDblClick,
        moveCursor: moveCursor,
        keyAddAtCursor: keyAddAtCursor,
        submitChain: submitChain,
        removeLastTile: removeLastTile,
        clearChain: clearChain,
        tryAddTile: tryAddTile,

        updateHUD: updateHUD,
        showTileLayer: showTileLayer,

        // Burning tile helpers
        sprinkleBurning: sprinkleBurning,
        descendBurning: descendBurning,
        forceBurnAt: forceBurnAt,

        // Pure helpers (also exposed through W.Board.Helpers)
        isValidPath: isValidPath,
        pathToWord: function (path) { return pathToWord(path, grid); },
        settle: function (g) { return settle(g); },
        findMatches: findMatches,

        // Test hooks
        setSeed: setSeed,
        makeEmptyGrid: makeEmptyGrid,
        newTile: newTile,
        fillGrid: fillGrid,
        cloneGrid: cloneGrid,
        popCells: popCells,
        playWordByPath: playWordByPath,
        triggerGameOver: triggerGameOver
    };
})();
