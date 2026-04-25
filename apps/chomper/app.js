// app.js — main game loop, Pac-Man entity, collisions, state
(function() {
"use strict";

var canvas = document.getElementById("game");
var ctx = canvas.getContext("2d");
var getW = function() { return Canvas.w(ctx, 900); };
var getH = function() { return Canvas.h(ctx, 900); };

// --- Game state ---
var STATE = "title"; // "title","playing","paused","dying","levelclear","gameover"
var score = 0;
var lives = 3;
var level = 1;
var frightenBlinkTimer = 0;
var frightBlinkOn = false;
var flashTimer = 0;
var flashText = "";
var ghostChainBonus = 0;  // for scoring successive ghost eats
var stateTimer = 0;        // countdown for dying/levelclear

// Pac-Man entity
var pac = {
    c: 0, r: 0,
    dir: 1,       // start facing left
    nextDir: -1,
    mouthAnim: 0,
    alive: true
};

function resetPac() {
    pac.c = P.Maze.pacmanSpawn.c;
    pac.r = P.Maze.pacmanSpawn.r;
    pac.dir = 1;
    pac.nextDir = -1;
    pac.mouthAnim = 0;
    pac.alive = true;
}

function startNewGame() {
    score = 0;
    lives = 3;
    level = 1;
    startLevel();
}

function startLevel() {
    P.Maze.reset();
    resetPac();
    P.Ghosts.init();
    frightenBlinkTimer = 0;
    ghostChainBonus = 0;
    STATE = "playing";
    showHUD(true);
    updateHUD();
    P.Screens.hideOverlay();
}

function showHUD(on) {
    document.getElementById("hud").style.display = on ? "block" : "none";
}

function updateHUD() {
    document.getElementById("hud-score").textContent = String(score);
    document.getElementById("hud-high").textContent = String(P.Storage.highScore);
    document.getElementById("hud-lives").textContent = String(lives);
    document.getElementById("hud-level").textContent = String(level);
}

function showCenter(text, duration) {
    flashText = text;
    flashTimer = duration;
    var el = document.getElementById("center-text");
    el.textContent = text;
    el.style.display = "block";
}

function hideCenter() {
    flashTimer = 0;
    document.getElementById("center-text").style.display = "none";
}

// --- Input routing ---
Input.onAction(function(action, phase) {
    if (phase !== "down" || !action) return;
    if (STATE === "title" || STATE === "gameover" || STATE === "paused") {
        if (action === "up")   P.Screens.menuUp();
        else if (action === "down") P.Screens.menuDown();
        else if (action === "confirm") {
            var menuAction = P.Screens.menuSelect();
            P.Audio.sfxMenu();
            handleMenuAction(menuAction);
        } else if (action === "pause" && STATE === "paused") {
            resumeGame();
        }
        return;
    }
    if (STATE === "playing" && action === "pause") pauseGame();
    // Directional actions update the input queue inside P.Input itself.
});

// Mouse hover/click on the overlay menus is handled by lib/screens; we
// only need to forward "click confirmed" to the same dispatcher used by
// keyboard confirms.
P.Screens.onConfirm = handleMenuAction;

function handleMenuAction(action) {
    if (!action) return;
    if (action === "play") {
        startNewGame();
    } else if (action === "quit") {
        // no-op; user closes window
    } else if (action === "title") {
        showTitle();
    } else if (action === "resume") {
        resumeGame();
    }
}

function showTitle() {
    STATE = "title";
    showHUD(false);
    hideCenter();
    P.Screens.setTitleHigh();
    P.Screens.switchTo("title");
}

function pauseGame() {
    if (STATE !== "playing") return;
    STATE = "paused";
    P.Screens.switchTo("paused");
}

function resumeGame() {
    if (STATE !== "paused") return;
    STATE = "playing";
    P.Screens.hideOverlay();
}

function gameOver() {
    STATE = "gameover";
    showHUD(false);
    var isNew = P.Storage.maybeSetHigh(score);
    P.Screens.setGameOverTitle("GAME OVER");
    P.Screens.setGameOverStats(score, isNew);
    P.Screens.switchTo("gameover");
}

function loseLife() {
    P.Audio.sfxDeath();
    lives--;
    updateHUD();
    STATE = "dying";
    stateTimer = 1400;
    pac.alive = false;
}

function afterDeath() {
    if (lives <= 0) {
        gameOver();
        return;
    }
    resetPac();
    P.Ghosts.resetAll();
    P.Input.reset();
    STATE = "playing";
}

function levelClear() {
    P.Audio.sfxWin();
    STATE = "levelclear";
    stateTimer = 1800;
    showCenter("LEVEL " + level + " CLEAR!", 1800);
}

function afterLevelClear() {
    level++;
    hideCenter();
    startLevel();
}

// --- Pac-Man movement ---
function canMove(c, r, dir) {
    var d = P.DIRS[dir];
    var nc = P.Maze.wrapCol(Math.round(c) + d.dx);
    var nr = Math.round(r) + d.dy;
    return P.Maze.isPassableForPac(nc, nr);
}

function updatePac(dt) {
    if (!pac.alive) return;
    var dtS = dt / 1000;
    var speed = 7.0; // tiles per second

    // Consume queued direction
    var queued = P.Input.consume();
    if (queued >= 0) pac.nextDir = queued;

    // Try to turn at tile center
    var ci = Math.round(pac.c);
    var ri = Math.round(pac.r);
    var step = speed * dtS;
    var atCenter = Math.abs(pac.c - ci) < step * 0.7 && Math.abs(pac.r - ri) < step * 0.7;

    if (atCenter && pac.nextDir >= 0 && pac.nextDir !== pac.dir) {
        if (canMove(ci, ri, pac.nextDir)) {
            pac.c = ci;
            pac.r = ri;
            pac.dir = pac.nextDir;
            pac.nextDir = -1;
        }
    }

    // Check forward
    var d = P.DIRS[pac.dir];
    var canGo = canMove(pac.c, pac.r, pac.dir);
    if (atCenter && !canGo) {
        pac.c = ci;
        pac.r = ri;
        // stop
        return;
    }

    pac.c += d.dx * step;
    pac.r += d.dy * step;
    pac.c = P.Maze.wrapCol(pac.c);
    // handle tunnel wrap crossing
    if (pac.c < -0.5) pac.c = P.Maze.COLS - 0.5;
    if (pac.c > P.Maze.COLS - 0.5) pac.c = -0.5;

    pac.mouthAnim += dt * 0.012;

    // Eat pellet at current tile
    var ec = Math.round(pac.c);
    var er = Math.round(pac.r);
    var eaten = P.Maze.eatPelletAt(ec, er);
    if (eaten === '.') {
        score += 10;
        P.Audio.sfxChomp();
        updateHUD();
    } else if (eaten === 'o') {
        score += 50;
        P.Audio.sfxPower();
        ghostChainBonus = 0;
        P.Ghosts.frightenAll(Math.max(3000, 8000 - (level - 1) * 500));
        updateHUD();
    }

    if (P.Maze.pelletCount <= 0) {
        levelClear();
    }
}

// --- Collisions ---
function checkCollisions() {
    if (!pac.alive) return;
    for (var i = 0; i < P.Ghosts.list.length; i++) {
        var g = P.Ghosts.list[i];
        if (g.mode === "eaten" || g.mode === "house" || g.mode === "leaving") continue;
        var dc = g.c - pac.c;
        var dr = g.r - pac.r;
        if (dc * dc + dr * dr < 0.55 * 0.55) {
            if (g.mode === "frightened") {
                ghostChainBonus++;
                var pts = 200 * Math.pow(2, ghostChainBonus - 1); // 200,400,800,1600
                if (pts > 1600) pts = 1600;
                score += pts;
                updateHUD();
                g.mode = "eaten";
                P.Audio.sfxEatGhost();
            } else {
                loseLife();
                return;
            }
        }
    }
}

// --- Rendering ---
function computeLayout(W, H) {
    // Leave room for HUD on top (~60px)
    var hudH = 60;
    var availH = H - hudH - 20;
    var availW = W - 40;
    var tile = Math.floor(Math.min(availW / P.Maze.COLS, availH / P.Maze.ROWS));
    var ox = Math.floor((W - tile * P.Maze.COLS) / 2);
    var oy = hudH + Math.floor((availH - tile * P.Maze.ROWS) / 2);
    return { tile: tile, ox: ox, oy: oy };
}

function drawPac(ctx, ox, oy, tile) {
    var cx = ox + pac.c * tile + tile / 2;
    var cy = oy + pac.r * tile + tile / 2;
    var rad = tile * 0.48;
    var mouthOpen = (Math.sin(pac.mouthAnim) + 1) * 0.5; // 0..1
    var angle = mouthOpen * 0.5;
    var facing = 0;
    if (pac.dir === 0) facing = 0;
    else if (pac.dir === 1) facing = Math.PI;
    else if (pac.dir === 2) facing = -Math.PI / 2;
    else facing = Math.PI / 2;

    ctx.fillStyle = "#ffff00";
    ctx.beginPath();
    ctx.moveTo(cx, cy);
    ctx.arc(cx, cy, rad, facing + angle, facing - angle + Math.PI * 2);
    ctx.closePath();
    ctx.fill();
}

function drawDying(ctx, ox, oy, tile, t) {
    // t: 0..1 progress of death anim
    var cx = ox + pac.c * tile + tile / 2;
    var cy = oy + pac.r * tile + tile / 2;
    var rad = tile * 0.48;
    var open = t * Math.PI;
    if (open > Math.PI) open = Math.PI;
    ctx.fillStyle = "#ffff00";
    ctx.beginPath();
    ctx.moveTo(cx, cy);
    ctx.arc(cx, cy, rad, -Math.PI / 2 + open, -Math.PI / 2 - open + Math.PI * 2);
    ctx.closePath();
    ctx.fill();
}

// --- Main loop ---
function frame(dt) {
    var W = getW(), H = getH();

    // Update
    if (STATE === "playing") {
        updatePac(dt);
        P.Ghosts.update(dt, pac);
        checkCollisions();

        // frightened blink near end
        var anyFright = false;
        var minTimer = Infinity;
        for (var i = 0; i < P.Ghosts.list.length; i++) {
            var g = P.Ghosts.list[i];
            if (g.mode === "frightened") {
                anyFright = true;
                if (g.frightenedTimer < minTimer) minTimer = g.frightenedTimer;
            }
        }
        if (anyFright && minTimer < 2000) {
            frightenBlinkTimer += dt;
            if (frightenBlinkTimer > 200) {
                frightenBlinkTimer = 0;
                frightBlinkOn = !frightBlinkOn;
            }
        } else {
            frightBlinkOn = false;
        }
    } else if (STATE === "dying") {
        stateTimer -= dt;
        if (stateTimer <= 0) afterDeath();
    } else if (STATE === "levelclear") {
        stateTimer -= dt;
        if (stateTimer <= 0) afterLevelClear();
    }

    if (flashTimer > 0) {
        flashTimer -= dt;
        if (flashTimer <= 0) hideCenter();
    }

    // Render
    ctx.fillStyle = "#000";
    ctx.fillRect(0, 0, W, H);

    if (STATE === "title" || STATE === "gameover") {
        return; // overlay handles visuals
    }

    var layout = computeLayout(W, H);
    P.Maze.draw(ctx, layout.ox, layout.oy, layout.tile);

    if (STATE === "dying") {
        var t = 1 - (stateTimer / 1400);
        drawDying(ctx, layout.ox, layout.oy, layout.tile, t);
    } else if (STATE === "levelclear") {
        // flash maze by altering alpha
        var phase = Math.floor(stateTimer / 200) % 2 === 0;
        if (phase) {
            ctx.fillStyle = "rgba(255,255,255,0.1)";
            ctx.fillRect(layout.ox, layout.oy, P.Maze.COLS * layout.tile, P.Maze.ROWS * layout.tile);
        }
        drawPac(ctx, layout.ox, layout.oy, layout.tile);
    } else {
        drawPac(ctx, layout.ox, layout.oy, layout.tile);
        P.Ghosts.draw(ctx, layout.ox, layout.oy, layout.tile, frightBlinkOn);
    }
}

// --- Init ---
P.Storage.load();
P.Audio.init();
P.Ghosts.init();
P.Maze.reset();
P.Screens.setTitleHigh();
P.Screens.switchTo("title");
showHUD(false);

GameLoop.create({ tick: frame, draw: function() {} }).start();

console.log("Pac-Man loaded!");
})();
