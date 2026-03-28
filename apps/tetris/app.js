// ============================================================
// Tetris — pure JS game logic, Canvas 2D rendering
// ============================================================

var ctx = document.getElementById("game").getContext("2d");
var W = ctx.canvasWidth;
var H = ctx.canvasHeight;

// --- Constants ---
var COLS = 10, ROWS = 20;
var CELL = Math.floor(Math.min(H / (ROWS + 4), W / (COLS + 8)));
var BOARD_W = COLS * CELL;
var BOARD_H = ROWS * CELL;
var BOARD_X = Math.floor((W - BOARD_W) / 2);
var BOARD_Y = Math.floor((H - BOARD_H) / 2);

// SDL key codes
var KEY_LEFT = 1073741904, KEY_RIGHT = 1073741903;
var KEY_DOWN = 1073741905, KEY_UP = 1073741906;
var KEY_SPACE = 32, KEY_ENTER = 13, KEY_P = 112;

// Piece colors
var COLORS = [
    null,
    "#00F0F0", // I - cyan
    "#F0F000", // O - yellow
    "#A000F0", // T - purple
    "#00F000", // S - green
    "#F00000", // Z - red
    "#0000F0", // J - blue
    "#F0A000"  // L - orange
];

var GHOST_COLORS = [
    null,
    "rgba(0,240,240,0.25)",
    "rgba(240,240,0,0.25)",
    "rgba(160,0,240,0.25)",
    "rgba(0,240,0,0.25)",
    "rgba(240,0,0,0.25)",
    "rgba(0,0,240,0.25)",
    "rgba(240,160,0,0.25)"
];

// Piece shapes: each is [4 rotations][cells as [row,col] offsets]
var PIECES = [
    null,
    // 1: I
    [[[1,0],[1,1],[1,2],[1,3]], [[0,2],[1,2],[2,2],[3,2]], [[2,0],[2,1],[2,2],[2,3]], [[0,1],[1,1],[2,1],[3,1]]],
    // 2: O
    [[[0,1],[0,2],[1,1],[1,2]], [[0,1],[0,2],[1,1],[1,2]], [[0,1],[0,2],[1,1],[1,2]], [[0,1],[0,2],[1,1],[1,2]]],
    // 3: T
    [[[0,1],[1,0],[1,1],[1,2]], [[0,1],[1,1],[1,2],[2,1]], [[1,0],[1,1],[1,2],[2,1]], [[0,1],[1,0],[1,1],[2,1]]],
    // 4: S
    [[[0,1],[0,2],[1,0],[1,1]], [[0,1],[1,1],[1,2],[2,2]], [[1,1],[1,2],[2,0],[2,1]], [[0,0],[1,0],[1,1],[2,1]]],
    // 5: Z
    [[[0,0],[0,1],[1,1],[1,2]], [[0,2],[1,1],[1,2],[2,1]], [[1,0],[1,1],[2,1],[2,2]], [[0,1],[1,0],[1,1],[2,0]]],
    // 6: J
    [[[0,0],[1,0],[1,1],[1,2]], [[0,1],[0,2],[1,1],[2,1]], [[1,0],[1,1],[1,2],[2,2]], [[0,1],[1,1],[2,0],[2,1]]],
    // 7: L
    [[[0,2],[1,0],[1,1],[1,2]], [[0,1],[1,1],[2,1],[2,2]], [[1,0],[1,1],[1,2],[2,0]], [[0,0],[0,1],[1,1],[2,1]]]
];

// --- Game state ---
var board = [];
for (var i = 0; i < ROWS; i++) {
    board[i] = [];
    for (var j = 0; j < COLS; j++) board[i][j] = 0;
}

var cur = null; // {type, x, y, rot}
var nextType = 0;
var score = 0, level = 1, totalLines = 0;
var gameState = 0; // 0=idle, 1=playing, 2=paused, 3=gameover
var dropTimer = null;

// HUD elements
var scoreEl = document.getElementById("score");
var levelEl = document.getElementById("level");
var linesEl = document.getElementById("lines");
var overlayEl = document.getElementById("overlay");
var overlayTitle = document.getElementById("overlay-title");
var overlaySub = document.getElementById("overlay-sub");

function updateHUD() {
    scoreEl.textContent = String(score);
    levelEl.textContent = String(level);
    linesEl.textContent = String(totalLines);
}

// --- Collision ---
function canPlace(type, x, y, rot) {
    var cells = PIECES[type][rot & 3];
    for (var i = 0; i < cells.length; i++) {
        var r = y + cells[i][0];
        var c = x + cells[i][1];
        if (c < 0 || c >= COLS || r >= ROWS) return false;
        if (r >= 0 && board[r][c] !== 0) return false;
    }
    return true;
}

function ghostY() {
    var gy = cur.y;
    while (canPlace(cur.type, cur.x, gy + 1, cur.rot)) gy++;
    return gy;
}

// --- Random piece ---
function randType() { return Math.floor(Math.random() * 7) + 1; }

// --- Spawn ---
function spawnPiece() {
    cur = { type: nextType, x: 3, y: -1, rot: 0 };
    nextType = randType();
    if (!canPlace(cur.type, cur.x, cur.y, cur.rot)) {
        gameOver();
    }
}

// --- Lock ---
function lockPiece() {
    var cells = PIECES[cur.type][cur.rot & 3];
    for (var i = 0; i < cells.length; i++) {
        var r = cur.y + cells[i][0];
        var c = cur.x + cells[i][1];
        if (r >= 0 && r < ROWS && c >= 0 && c < COLS) {
            board[r][c] = cur.type;
        }
    }
}

// --- Clear lines ---
function clearLines() {
    var cleared = 0;
    for (var r = ROWS - 1; r >= 0; r--) {
        var full = true;
        for (var c = 0; c < COLS; c++) {
            if (board[r][c] === 0) { full = false; break; }
        }
        if (full) {
            board.splice(r, 1);
            var emptyRow = [];
            for (var c = 0; c < COLS; c++) emptyRow.push(0);
            board.unshift(emptyRow);
            cleared++;
            r++; // recheck
        }
    }
    if (cleared > 0) {
        var pts = [0, 40, 100, 300, 1200];
        score += pts[cleared] * level;
        totalLines += cleared;
        var newLevel = Math.floor(totalLines / 10) + 1;
        if (newLevel !== level) {
            level = newLevel;
            resetDrop();
        }
        updateHUD();
    }
}

// --- Movement ---
function moveLeft() { if (canPlace(cur.type, cur.x - 1, cur.y, cur.rot)) cur.x--; }
function moveRight() { if (canPlace(cur.type, cur.x + 1, cur.y, cur.rot)) cur.x++; }

function moveDown() {
    if (canPlace(cur.type, cur.x, cur.y + 1, cur.rot)) {
        cur.y++;
        return true;
    }
    lockPiece();
    clearLines();
    spawnPiece();
    return false;
}

function rotate() {
    var nr = (cur.rot + 1) & 3;
    if (canPlace(cur.type, cur.x, cur.y, nr)) { cur.rot = nr; return; }
    if (canPlace(cur.type, cur.x - 1, cur.y, nr)) { cur.x--; cur.rot = nr; return; }
    if (canPlace(cur.type, cur.x + 1, cur.y, nr)) { cur.x++; cur.rot = nr; return; }
    if (canPlace(cur.type, cur.x - 2, cur.y, nr)) { cur.x -= 2; cur.rot = nr; return; }
    if (canPlace(cur.type, cur.x + 2, cur.y, nr)) { cur.x += 2; cur.rot = nr; }
}

function hardDrop() {
    var gy = ghostY();
    score += (gy - cur.y) * 2;
    cur.y = gy;
    lockPiece();
    clearLines();
    spawnPiece();
    updateHUD();
}

// --- Drop timer ---
function getSpeed() {
    var speeds = [800,720,630,550,470,380,300,220,140,100,80,80,80,70,70,70,50,50,50,30];
    var idx = level - 1;
    if (idx >= speeds.length) idx = speeds.length - 1;
    return speeds[idx];
}

function resetDrop() {
    if (dropTimer !== null) clearInterval(dropTimer);
    dropTimer = setInterval(function() {
        if (gameState === 1) {
            moveDown();
            draw();
        }
    }, getSpeed());
}

// --- Game lifecycle ---
function startGame() {
    for (var r = 0; r < ROWS; r++)
        for (var c = 0; c < COLS; c++)
            board[r][c] = 0;
    score = 0; level = 1; totalLines = 0;
    gameState = 1;
    updateHUD();
    overlayEl.style.display = "none";
    nextType = randType();
    spawnPiece();
    resetDrop();
    draw();
}

function gameOver() {
    gameState = 3;
    cur = null;
    if (dropTimer !== null) { clearInterval(dropTimer); dropTimer = null; }
    overlayTitle.textContent = "GAME OVER";
    overlaySub.textContent = "Score: " + score + "  Press ENTER to restart";
    overlayEl.style.display = "flex";
    draw();
}

// --- Drawing ---
function drawCell(col, row, color) {
    ctx.fillStyle = color;
    ctx.fillRect(BOARD_X + col * CELL + 1, BOARD_Y + row * CELL + 1, CELL - 2, CELL - 2);
}

function drawPiece(type, x, y, rot, colors) {
    var cells = PIECES[type][rot & 3];
    for (var i = 0; i < cells.length; i++) {
        var r = y + cells[i][0];
        var c = x + cells[i][1];
        if (r >= 0) drawCell(c, r, colors[type]);
    }
}

function draw() {
    // Recalculate dimensions in case of resize
    W = ctx.canvasWidth;
    H = ctx.canvasHeight;
    CELL = Math.floor(Math.min(H / (ROWS + 4), W / (COLS + 8)));
    BOARD_W = COLS * CELL;
    BOARD_H = ROWS * CELL;
    BOARD_X = Math.floor((W - BOARD_W) / 2);
    BOARD_Y = Math.floor((H - BOARD_H) / 2);

    ctx.clearRect(0, 0, W, H);

    // Board background
    ctx.fillStyle = "#0a0a0f";
    ctx.fillRect(BOARD_X, BOARD_Y, BOARD_W, BOARD_H);

    // Grid
    ctx.strokeStyle = "#1e1e28";
    for (var c = 0; c <= COLS; c++) {
        var x = BOARD_X + c * CELL;
        ctx.strokeRect(x, BOARD_Y, 0, BOARD_H);
    }
    for (var r = 0; r <= ROWS; r++) {
        var y = BOARD_Y + r * CELL;
        ctx.strokeRect(BOARD_X, y, BOARD_W, 0);
    }

    // Filled cells
    for (var r = 0; r < ROWS; r++) {
        for (var c = 0; c < COLS; c++) {
            if (board[r][c] !== 0) drawCell(c, r, COLORS[board[r][c]]);
        }
    }

    // Ghost piece
    if (cur && gameState === 1) {
        var gy = ghostY();
        drawPiece(cur.type, cur.x, gy, cur.rot, GHOST_COLORS);
    }

    // Current piece
    if (cur && gameState === 1) {
        drawPiece(cur.type, cur.x, cur.y, cur.rot, COLORS);
    }

    // Next piece preview
    var pvX = BOARD_X + BOARD_W + CELL * 2;
    var pvY = BOARD_Y + CELL;
    ctx.fillStyle = "#141420";
    ctx.fillRect(pvX - CELL, pvY - CELL, CELL * 6, CELL * 6);
    ctx.strokeStyle = "#333";
    ctx.strokeRect(pvX - CELL, pvY - CELL, CELL * 6, CELL * 6);

    if (nextType > 0) {
        var cells = PIECES[nextType][0];
        for (var i = 0; i < cells.length; i++) {
            ctx.fillStyle = COLORS[nextType];
            ctx.fillRect(pvX + cells[i][1] * CELL + 1, pvY + cells[i][0] * CELL + 1,
                         CELL - 2, CELL - 2);
        }
    }

    // Board border
    ctx.strokeStyle = "#555";
    ctx.strokeRect(BOARD_X - 1, BOARD_Y - 1, BOARD_W + 2, BOARD_H + 2);
}

// --- Input ---
document.body.addEventListener("keydown", function(e) {
    var key = parseInt(e.key);

    if (gameState === 0 || gameState === 3) {
        if (key === KEY_ENTER) startGame();
        return;
    }
    if (gameState !== 1) return;

    if (key === KEY_LEFT) moveLeft();
    else if (key === KEY_RIGHT) moveRight();
    else if (key === KEY_DOWN) moveDown();
    else if (key === KEY_UP) rotate();
    else if (key === KEY_SPACE) hardDrop();

    draw();
});

// Initial draw
draw();
console.log("Tetris loaded!");
