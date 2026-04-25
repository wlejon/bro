// board.js — Tetris game logic, piece data, layout, drawing helpers
var T = T || {};

// Piece shapes: [type 1-7][4 rotations][cells as [row,col]]
T.PIECES = [
    null,
    // I
    [[[1,0],[1,1],[1,2],[1,3]],[[0,2],[1,2],[2,2],[3,2]],
     [[2,0],[2,1],[2,2],[2,3]],[[0,1],[1,1],[2,1],[3,1]]],
    // O
    [[[0,1],[0,2],[1,1],[1,2]],[[0,1],[0,2],[1,1],[1,2]],
     [[0,1],[0,2],[1,1],[1,2]],[[0,1],[0,2],[1,1],[1,2]]],
    // T
    [[[0,1],[1,0],[1,1],[1,2]],[[0,1],[1,1],[1,2],[2,1]],
     [[1,0],[1,1],[1,2],[2,1]],[[0,1],[1,0],[1,1],[2,1]]],
    // S
    [[[0,1],[0,2],[1,0],[1,1]],[[0,1],[1,1],[1,2],[2,2]],
     [[1,1],[1,2],[2,0],[2,1]],[[0,0],[1,0],[1,1],[2,1]]],
    // Z
    [[[0,0],[0,1],[1,1],[1,2]],[[0,2],[1,1],[1,2],[2,1]],
     [[1,0],[1,1],[2,1],[2,2]],[[0,1],[1,0],[1,1],[2,0]]],
    // J
    [[[0,0],[1,0],[1,1],[1,2]],[[0,1],[0,2],[1,1],[2,1]],
     [[1,0],[1,1],[1,2],[2,2]],[[0,1],[1,1],[2,0],[2,1]]],
    // L
    [[[0,2],[1,0],[1,1],[1,2]],[[0,1],[1,1],[2,1],[2,2]],
     [[1,0],[1,1],[1,2],[2,0]],[[0,0],[0,1],[1,1],[2,1]]]
];

T.COLORS = [null,"#00e5ff","#ffd600","#aa00ff","#00e676","#ff1744","#2979ff","#ff9100"];
T.COLORS_LIGHT = [null,"#4df0ff","#ffeb3b","#d050ff","#69f0ae","#ff5252","#448aff","#ffab40"];
T.COLORS_DARK = [null,"#006978","#7f6b00","#55007f","#007a3b","#7f0b22","#143f7f","#7f4800"];

T.KICKS = {
    normal: [
        [[0,0],[-1,0],[-1,1],[0,-2],[-1,-2]],
        [[0,0],[1,0],[1,-1],[0,2],[1,2]],
        [[0,0],[1,0],[1,1],[0,-2],[1,-2]],
        [[0,0],[-1,0],[-1,-1],[0,2],[-1,2]]
    ],
    I: [
        [[0,0],[-2,0],[1,0],[-2,-1],[1,2]],
        [[0,0],[-1,0],[2,0],[-1,2],[2,-1]],
        [[0,0],[2,0],[-1,0],[2,1],[-1,-2]],
        [[0,0],[1,0],[-2,0],[1,-2],[-2,1]]
    ]
};

T.SPEEDS = [800,717,633,550,467,383,300,217,133,100,83,83,83,67,67,67,50,50,50,33];

T.Board = {
    COLS: 10, ROWS: 20,
    CELL: 0, BOARD_W: 0, BOARD_H: 0, BOARD_X: 0, BOARD_Y: 0,

    // Game state
    board: [],
    cur: null,
    nextTypes: [],
    holdType: 0,
    holdUsed: false,
    score: 0,
    level: 1,
    totalLines: 0,
    combo: -1,
    backToBack: false,
    gameTime: 0,
    piecesPlaced: 0,

    // Mode
    mode: "marathon",
    modeTimer: 0,
    finished: false,

    // Stats
    stats: { singles: 0, doubles: 0, triples: 0, tetrises: 0, maxCombo: 0 },

    // Timing
    dropTimer: 0,
    dropInterval: 0,
    lockTimer: 0,
    lockDelay: 500,
    lockMoves: 0,
    maxLockMoves: 15,

    // Bag
    bag: [],

    calcLayout: function(W, H) {
        this.CELL = Math.floor(Math.min(H / (this.ROWS + 4), W / (this.COLS + 10)));
        this.BOARD_W = this.COLS * this.CELL;
        this.BOARD_H = this.ROWS * this.CELL;
        this.BOARD_X = Math.floor((W - this.BOARD_W) / 2);
        this.BOARD_Y = Math.floor((H - this.BOARD_H) / 2);
    },

    resetBoard: function() {
        this.board = [];
        for (var i = 0; i < this.ROWS; i++) {
            this.board[i] = [];
            for (var j = 0; j < this.COLS; j++) this.board[i][j] = 0;
        }
    },

    refillBag: function() {
        this.bag = [1, 2, 3, 4, 5, 6, 7];
        for (var i = this.bag.length - 1; i > 0; i--) {
            var j = Math.floor(Math.random() * (i + 1));
            var tmp = this.bag[i]; this.bag[i] = this.bag[j]; this.bag[j] = tmp;
        }
    },

    nextPiece: function() {
        if (this.bag.length === 0) this.refillBag();
        return this.bag.pop();
    },

    ensureNextTypes: function() {
        while (this.nextTypes.length < 5) this.nextTypes.push(this.nextPiece());
    },

    canPlace: function(type, x, y, rot) {
        var cells = T.PIECES[type][rot & 3];
        for (var i = 0; i < cells.length; i++) {
            var r = y + cells[i][0], c = x + cells[i][1];
            if (c < 0 || c >= this.COLS || r >= this.ROWS) return false;
            if (r >= 0 && this.board[r][c] !== 0) return false;
        }
        return true;
    },

    ghostY: function() {
        if (!this.cur) return 0;
        var gy = this.cur.y;
        while (this.canPlace(this.cur.type, this.cur.x, gy + 1, this.cur.rot)) gy++;
        return gy;
    },

    spawnPiece: function() {
        this.ensureNextTypes();
        var type = this.nextTypes.shift();
        this.ensureNextTypes();
        this.cur = { type: type, x: 3, y: -1, rot: 0 };
        this.holdUsed = false;
        this.dropTimer = 0;
        this.lockTimer = 0;
        this.lockMoves = 0;
        if (!this.canPlace(type, 3, -1, 0)) {
            this.cur.y = -2;
            if (!this.canPlace(type, 3, -2, 0)) return false;
        }
        return true;
    },

    resetLockTimer: function() {
        if (this.lockMoves < this.maxLockMoves) {
            this.lockTimer = 0;
            this.lockMoves++;
        }
    },

    lockPiece: function() {
        if (!this.cur) return;
        var cells = T.PIECES[this.cur.type][this.cur.rot & 3];
        var lockedOut = false;
        for (var i = 0; i < cells.length; i++) {
            var r = this.cur.y + cells[i][0], c = this.cur.x + cells[i][1];
            if (r < 0) {
                lockedOut = true;
            } else if (r < this.ROWS && c >= 0 && c < this.COLS) {
                this.board[r][c] = this.cur.type;
                T.FX.flash(r, c, 200, T.COLORS_LIGHT[this.cur.type]);
            }
        }
        this.piecesPlaced++;
        T.Audio.sfxLock();
        if (lockedOut) return -1; // piece locked above the board — top out
        return this.clearLines();
    },

    clearLines: function() {
        var cleared = [];
        for (var r = this.ROWS - 1; r >= 0; r--) {
            var full = true;
            for (var c = 0; c < this.COLS; c++) {
                if (this.board[r][c] === 0) { full = false; break; }
            }
            if (full) cleared.push(r);
        }

        if (cleared.length === 0) { this.combo = -1; return 0; }

        this.combo++;
        if (this.combo > this.stats.maxCombo) this.stats.maxCombo = this.combo;

        // Track stats
        if (cleared.length === 1) this.stats.singles++;
        else if (cleared.length === 2) this.stats.doubles++;
        else if (cleared.length === 3) this.stats.triples++;
        else if (cleared.length === 4) this.stats.tetrises++;

        // Scoring
        var pts = [0, 100, 300, 500, 800];
        var baseScore = pts[cleared.length] * this.level;
        var isTetris = (cleared.length === 4);
        if (isTetris) {
            if (this.backToBack) baseScore = Math.floor(baseScore * 1.5);
            this.backToBack = true;
        } else {
            this.backToBack = false;
        }
        if (this.combo > 0) {
            baseScore += 50 * this.combo * this.level;
            T.Audio.sfxCombo(this.combo);
        }
        this.score += baseScore;
        this.totalLines += cleared.length;

        // Level up
        var newLevel = Math.floor(this.totalLines / 10) + T.Storage.settings.startLevel;
        if (newLevel !== this.level) {
            this.level = newLevel;
            T.Audio.sfxLevelUp();
            T.FX.showText("LEVEL " + this.level);
            T.Audio.checkSongChange(this.level);
            T.Audio.updateMusicBPM(this.level);
        }

        // SFX + effects
        if (cleared.length === 4) {
            T.Audio.sfxTetris();
            T.FX.showText("QUAD!");
            T.FX.shake(300, 8);
        } else if (cleared.length === 3) {
            T.Audio.sfxClear3();
            T.FX.showText("TRIPLE");
        } else if (cleared.length === 2) {
            T.Audio.sfxClear2();
            T.FX.showText("DOUBLE");
        } else {
            T.Audio.sfxClear1();
        }
        if (this.combo > 1) T.FX.showText(this.combo + "x COMBO!");

        T.FX.startLineClear(cleared);

        // Particles
        for (var ri = 0; ri < cleared.length; ri++) {
            var row = cleared[ri];
            for (var c = 0; c < this.COLS; c++) {
                var color = T.COLORS[this.board[row][c]] || "#fff";
                T.FX.spawn(
                    this.BOARD_X + c * this.CELL + this.CELL / 2,
                    this.BOARD_Y + row * this.CELL + this.CELL / 2,
                    3, color
                );
            }
        }

        // Remove lines
        for (var ri = 0; ri < cleared.length; ri++) {
            this.board.splice(cleared[ri], 1);
            var emptyRow = [];
            for (var c = 0; c < this.COLS; c++) emptyRow.push(0);
            this.board.unshift(emptyRow);
            for (var rj = ri + 1; rj < cleared.length; rj++) {
                if (cleared[rj] < cleared[ri]) cleared[rj]++;
            }
        }

        return cleared.length;
    },

    // Movement
    moveLeft: function() {
        if (!this.cur) return false;
        if (this.canPlace(this.cur.type, this.cur.x - 1, this.cur.y, this.cur.rot)) {
            this.cur.x--; this.resetLockTimer(); T.Audio.sfxMove(); return true;
        }
        return false;
    },

    moveRight: function() {
        if (!this.cur) return false;
        if (this.canPlace(this.cur.type, this.cur.x + 1, this.cur.y, this.cur.rot)) {
            this.cur.x++; this.resetLockTimer(); T.Audio.sfxMove(); return true;
        }
        return false;
    },

    moveDown: function() {
        if (!this.cur) return false;
        if (this.canPlace(this.cur.type, this.cur.x, this.cur.y + 1, this.cur.rot)) {
            this.cur.y++; return true;
        }
        return false;
    },

    rotateCW: function() {
        if (this.cur) this.tryRotate((this.cur.rot + 1) & 3);
    },

    rotateCCW: function() {
        if (this.cur) this.tryRotate((this.cur.rot + 3) & 3);
    },

    tryRotate: function(newRot) {
        if (!this.cur) return;
        var kickData = (this.cur.type === 1) ? T.KICKS.I : T.KICKS.normal;
        var kickSet = kickData[this.cur.rot];
        for (var i = 0; i < kickSet.length; i++) {
            var dx = kickSet[i][0], dy = -kickSet[i][1];
            if (this.canPlace(this.cur.type, this.cur.x + dx, this.cur.y + dy, newRot)) {
                this.cur.x += dx;
                this.cur.y += dy;
                this.cur.rot = newRot;
                this.resetLockTimer();
                T.Audio.sfxRotate();
                return;
            }
        }
    },

    hardDrop: function() {
        if (!this.cur) return;
        var gy = this.ghostY();
        var dropDist = gy - this.cur.y;
        this.score += dropDist * 2;

        // Trail particles
        var cells = T.PIECES[this.cur.type][this.cur.rot & 3];
        for (var i = 0; i < cells.length; i++) {
            var c = this.cur.x + cells[i][1];
            for (var r = this.cur.y + cells[i][0]; r <= gy + cells[i][0]; r++) {
                if (r >= 0 && r < this.ROWS) {
                    T.FX.flash(r, c, 120, T.COLORS[this.cur.type]);
                }
            }
        }

        // Impact particles at landing
        for (var i = 0; i < cells.length; i++) {
            T.FX.spawn(
                this.BOARD_X + (this.cur.x + cells[i][1]) * this.CELL + this.CELL / 2,
                this.BOARD_Y + (gy + cells[i][0]) * this.CELL + this.CELL / 2,
                2, T.COLORS[this.cur.type], { spread: 3, spreadY: 2, life: 200, lifeVar: 100 }
            );
        }
        if (dropDist > 4) T.FX.shake(120, 3);

        this.cur.y = gy;
        T.Audio.sfxDrop();
        var lockResult = this.lockPiece();
        if (lockResult === -1) return false;
        if (!this.spawnPiece()) return false;
        return true;
    },

    doHold: function() {
        if (!this.cur || this.holdUsed) return;
        T.Audio.sfxHold();
        var type = this.cur.type;
        if (this.holdType === 0) {
            this.holdType = type;
            this.spawnPiece();
        } else {
            var tmp = this.holdType;
            this.holdType = type;
            this.cur = { type: tmp, x: 3, y: -1, rot: 0 };
            this.dropTimer = 0;
            this.lockTimer = 0;
            this.lockMoves = 0;
        }
        this.holdUsed = true;
    },

    getDropInterval: function() {
        var idx = this.level - 1;
        if (idx < 0) idx = 0;
        if (idx >= T.SPEEDS.length) idx = T.SPEEDS.length - 1;
        return T.SPEEDS[idx];
    },

    startGame: function(mode) {
        this.mode = mode || "marathon";
        this.resetBoard();
        this.score = 0;
        this.level = T.Storage.settings.startLevel;
        this.totalLines = 0;
        this.combo = -1;
        this.backToBack = false;
        this.holdType = 0;
        this.holdUsed = false;
        this.gameTime = 0;
        this.piecesPlaced = 0;
        this.finished = false;
        this.modeTimer = (mode === "ultra") ? 120000 : 0;
        this.stats = { singles: 0, doubles: 0, triples: 0, tetrises: 0, maxCombo: 0 };
        this.bag = [];
        this.nextTypes = [];
        this.dropTimer = 0;
        this.lockTimer = 0;
        this.lockMoves = 0;
        T.FX.clear();
        T.Input.resetDAS();
        this.refillBag();
        this.ensureNextTypes();
        this.spawnPiece();
        this.dropInterval = this.getDropInterval();
    },

    // Check mode-specific end conditions; returns true if game should end
    checkModeEnd: function() {
        if (this.mode === "sprint" && this.totalLines >= 40) {
            this.finished = true;
            return true;
        }
        if (this.mode === "ultra" && this.modeTimer <= 0) {
            this.finished = true;
            return true;
        }
        return false;
    },

    // Drawing helpers
    drawCell: function(ctx, col, row, color, alpha) {
        ctx.globalAlpha = alpha !== undefined ? alpha : 1.0;
        ctx.fillStyle = color;
        var x = this.BOARD_X + col * this.CELL + 1;
        var y = this.BOARD_Y + row * this.CELL + 1;
        var s = this.CELL - 2;
        ctx.fillRect(x, y, s, s);
        ctx.fillStyle = "rgba(255,255,255,0.15)";
        ctx.fillRect(x, y, s, 2);
        ctx.fillRect(x, y, 2, s);
        ctx.fillStyle = "rgba(0,0,0,0.2)";
        ctx.fillRect(x, y + s - 2, s, 2);
        ctx.fillRect(x + s - 2, y, 2, s);
        ctx.globalAlpha = 1.0;
    },

    drawPiece: function(ctx, type, x, y, rot, alpha) {
        var cells = T.PIECES[type][rot & 3];
        for (var i = 0; i < cells.length; i++) {
            var r = y + cells[i][0], c = x + cells[i][1];
            if (r >= 0) this.drawCell(ctx, c, r, T.COLORS[type], alpha);
        }
    },

    drawMiniPiece: function(ctx, type, px, py, cellSize) {
        if (type <= 0) return;
        var cells = T.PIECES[type][0];
        var minC = 9, maxC = 0, minR = 9, maxR = 0;
        for (var i = 0; i < cells.length; i++) {
            if (cells[i][1] < minC) minC = cells[i][1];
            if (cells[i][1] > maxC) maxC = cells[i][1];
            if (cells[i][0] < minR) minR = cells[i][0];
            if (cells[i][0] > maxR) maxR = cells[i][0];
        }
        var pw = (maxC - minC + 1) * cellSize;
        var ph = (maxR - minR + 1) * cellSize;
        var ox = px + (cellSize * 4 - pw) / 2 - minC * cellSize;
        var oy = py + (cellSize * 3 - ph) / 2 - minR * cellSize;
        ctx.fillStyle = T.COLORS[type];
        for (var i = 0; i < cells.length; i++) {
            var cx = ox + cells[i][1] * cellSize;
            var cy = oy + cells[i][0] * cellSize;
            ctx.fillRect(cx + 1, cy + 1, cellSize - 2, cellSize - 2);
        }
    },

    drawBoard: function(ctx) {
        var B = this;
        // Background
        ctx.fillStyle = "#08080e";
        ctx.fillRect(B.BOARD_X, B.BOARD_Y, B.BOARD_W, B.BOARD_H);

        // Grid
        if (T.Storage.settings.gridLines) {
            ctx.strokeStyle = "#181822";
            for (var c = 0; c <= B.COLS; c++) ctx.strokeRect(B.BOARD_X + c * B.CELL, B.BOARD_Y, 0, B.BOARD_H);
            for (var r = 0; r <= B.ROWS; r++) ctx.strokeRect(B.BOARD_X, B.BOARD_Y + r * B.CELL, B.BOARD_W, 0);
        }

        // Filled cells
        for (var r = 0; r < B.ROWS; r++) {
            if (!B.board[r]) continue;
            for (var c = 0; c < B.COLS; c++) {
                if (B.board[r][c] !== 0) B.drawCell(ctx, c, r, T.COLORS[B.board[r][c]]);
            }
        }

        // FX layers
        T.FX.drawFlashCells(ctx, B.BOARD_X, B.BOARD_Y, B.CELL);
        T.FX.drawLineClearFlash(ctx, B.BOARD_X, B.BOARD_Y, B.BOARD_W, B.CELL);

        // Ghost
        if (B.cur && T.Storage.settings.ghostPiece) {
            var gy = B.ghostY();
            if (gy !== B.cur.y) {
                var cells = T.PIECES[B.cur.type][B.cur.rot & 3];
                for (var i = 0; i < cells.length; i++) {
                    var r = gy + cells[i][0], c = B.cur.x + cells[i][1];
                    if (r >= 0) {
                        ctx.globalAlpha = 0.2;
                        ctx.fillStyle = T.COLORS[B.cur.type];
                        ctx.fillRect(B.BOARD_X + c * B.CELL + 1, B.BOARD_Y + r * B.CELL + 1, B.CELL - 2, B.CELL - 2);
                        ctx.globalAlpha = 0.4;
                        ctx.strokeStyle = T.COLORS[B.cur.type];
                        ctx.strokeRect(B.BOARD_X + c * B.CELL + 1, B.BOARD_Y + r * B.CELL + 1, B.CELL - 2, B.CELL - 2);
                        ctx.globalAlpha = 1.0;
                    }
                }
            }
        }

        // Current piece
        if (B.cur) {
            var lockAlpha = 1.0;
            if (!B.canPlace(B.cur.type, B.cur.x, B.cur.y + 1, B.cur.rot) && B.lockTimer > 0) {
                lockAlpha = 1.0 - (B.lockTimer / B.lockDelay) * 0.3;
            }
            B.drawPiece(ctx, B.cur.type, B.cur.x, B.cur.y, B.cur.rot, lockAlpha);
        }

        // Border
        ctx.strokeStyle = "#444";
        ctx.strokeRect(B.BOARD_X - 1, B.BOARD_Y - 1, B.BOARD_W + 2, B.BOARD_H + 2);
    },

    drawPreviews: function(ctx) {
        var B = this;
        var pvCell = Math.floor(B.CELL * 0.7);

        // Hold
        var hx = B.BOARD_X - pvCell * 5 - 10, hy = B.BOARD_Y;
        ctx.fillStyle = "#0c0c14";
        ctx.fillRect(hx, hy, pvCell * 4 + 8, pvCell * 3 + 8);
        ctx.strokeStyle = "#333";
        ctx.strokeRect(hx, hy, pvCell * 4 + 8, pvCell * 3 + 8);
        if (B.holdType > 0) {
            ctx.globalAlpha = B.holdUsed ? 0.4 : 1.0;
            B.drawMiniPiece(ctx, B.holdType, hx + 4, hy + 4, pvCell);
            ctx.globalAlpha = 1.0;
        }

        // Next + queue
        var nx = B.BOARD_X + B.BOARD_W + 10, ny = B.BOARD_Y;
        ctx.fillStyle = "#0c0c14";
        ctx.fillRect(nx, ny, pvCell * 4 + 8, pvCell * 3 + 8);
        ctx.strokeStyle = "#333";
        ctx.strokeRect(nx, ny, pvCell * 4 + 8, pvCell * 3 + 8);
        if (B.nextTypes.length > 0) {
            B.drawMiniPiece(ctx, B.nextTypes[0], nx + 4, ny + 4, pvCell);
        }
        for (var qi = 1; qi < Math.min(B.nextTypes.length, 4); qi++) {
            var qy = ny + (pvCell * 3 + 16) * qi + 8;
            ctx.fillStyle = "#0a0a10";
            ctx.fillRect(nx, qy, pvCell * 4 + 8, pvCell * 3 + 8);
            ctx.strokeStyle = "#222";
            ctx.strokeRect(nx, qy, pvCell * 4 + 8, pvCell * 3 + 8);
            ctx.globalAlpha = 0.6;
            B.drawMiniPiece(ctx, B.nextTypes[qi], nx + 4, qy + 4, pvCell);
            ctx.globalAlpha = 1.0;
        }
    },

    updateHUD: function() {
        var B = this;
        var el;
        el = document.getElementById("hud-score");
        if (el) el.textContent = String(B.score);
        el = document.getElementById("hud-level");
        if (el) el.textContent = String(B.level);
        el = document.getElementById("hud-lines");
        if (el) el.textContent = String(B.totalLines);

        // Combo
        var comboLabel = document.getElementById("hud-combo-label");
        var comboVal = document.getElementById("hud-combo");
        if (comboLabel && comboVal) {
            if (B.combo > 0) {
                comboLabel.style.display = "block";
                comboVal.style.display = "block";
                comboVal.textContent = String(B.combo);
            } else {
                comboLabel.style.display = "none";
                comboVal.style.display = "none";
            }
        }

        // Mode-specific extra info
        var extraLabel = document.getElementById("hud-extra-label");
        var extraVal = document.getElementById("hud-extra");
        if (extraLabel && extraVal) {
            if (B.mode === "sprint") {
                extraLabel.style.display = "block";
                extraVal.style.display = "block";
                var remaining = Math.max(0, 40 - B.totalLines);
                extraLabel.textContent = "LEFT";
                extraVal.textContent = String(remaining);
            } else if (B.mode === "ultra") {
                extraLabel.style.display = "block";
                extraVal.style.display = "block";
                var remain = Math.max(0, Math.ceil(B.modeTimer / 1000));
                var mins = Math.floor(remain / 60);
                var secs = remain % 60;
                extraLabel.textContent = "TIME";
                extraVal.textContent = mins + ":" + (secs < 10 ? "0" : "") + secs;
            } else {
                extraLabel.style.display = "none";
                extraVal.style.display = "none";
            }
        }
    },

    formatTime: function(ms) {
        var totalSecs = Math.floor(ms / 1000);
        var mins = Math.floor(totalSecs / 60);
        var secs = totalSecs % 60;
        var centis = Math.floor((ms % 1000) / 10);
        return mins + ":" + (secs < 10 ? "0" : "") + secs + "." + (centis < 10 ? "0" : "") + centis;
    }
};
