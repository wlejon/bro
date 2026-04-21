// app.js — Classic Snake for the bro runtime
(function() {
"use strict";

// ---------- Storage ----------
var Storage = {
    highScore: 0,
    load: function() {
        try {
            var s = localStorage.getItem("snake_highscore");
            if (s !== null) this.highScore = parseInt(s, 10) || 0;
        } catch(e) {}
    },
    save: function() {
        try { localStorage.setItem("snake_highscore", String(this.highScore)); } catch(e) {}
    }
};
Storage.load();

// ---------- Audio (optional, minimal) ----------
var Audio = {
    ctx: null,
    init: function() {
        try { this.ctx = new AudioContext(); } catch(e) { this.ctx = null; }
    },
    tone: function(freq, duration, type, vol) {
        if (!this.ctx) return;
        try {
            var id = this.ctx.createVoice();
            this.ctx.setVoiceWaveform(id, type || "square");
            this.ctx.setVoiceFrequency(id, freq);
            this.ctx.setVoiceGain(id, (vol || 0.5) * 12.0);
            this.ctx.setVoiceAttack(id, 0.003);
            this.ctx.setVoiceDecay(id, duration * 0.8);
            this.ctx.setVoiceSustain(id, 0.0);
            this.ctx.setVoiceRelease(id, 0.03);
            var t = this.ctx.currentTime;
            this.ctx.startVoice(id, t);
            this.ctx.stopVoice(id, t + duration);
        } catch(e) {}
    },
    sfxEat:    function() { this.tone(660, 0.08, "square", 0.6); },
    sfxDie:    function() {
        var a = this;
        a.tone(300, 0.15, "sawtooth", 0.5);
        setTimeout(function(){ a.tone(200, 0.2, "sawtooth", 0.5); }, 140);
        setTimeout(function(){ a.tone(120, 0.3, "sawtooth", 0.5); }, 300);
    },
    sfxMenu:   function() { this.tone(440, 0.04, "sine", 0.3); },
    sfxSelect: function() { this.tone(660, 0.07, "square", 0.4); }
};
Audio.init();

// ---------- Canvas ----------
var canvas = document.getElementById("game");
var ctx = canvas.getContext("2d");
var getW = function() { return ctx.canvasWidth || canvas.width || 800; };
var getH = function() { return ctx.canvasHeight || canvas.height || 700; };

// ---------- Game constants ----------
var COLS = 28;
var ROWS = 22;
var TICK_MIN = 55;   // ms per step at max speed
var TICK_MAX = 140;  // ms per step at start

// ---------- Game state ----------
var game = null;

function createGame() {
    var g = {
        grid: { cols: COLS, rows: ROWS },
        snake: [],           // array of {x,y}; index 0 is head
        dir: { x: 1, y: 0 },
        nextDir: { x: 1, y: 0 },
        food: { x: 0, y: 0 },
        growPending: 0,
        score: 0,
        stepInterval: TICK_MAX,
        stepTimer: 0,
        alive: true,
        flashTimer: 0
    };
    var cx = Math.floor(COLS / 2);
    var cy = Math.floor(ROWS / 2);
    g.snake.push({ x: cx,     y: cy });
    g.snake.push({ x: cx - 1, y: cy });
    g.snake.push({ x: cx - 2, y: cy });
    placeFood(g);
    return g;
}

function placeFood(g) {
    // Pick random empty cell
    for (var tries = 0; tries < 500; tries++) {
        var x = Math.floor(Math.random() * g.grid.cols);
        var y = Math.floor(Math.random() * g.grid.rows);
        var onSnake = false;
        for (var i = 0; i < g.snake.length; i++) {
            if (g.snake[i].x === x && g.snake[i].y === y) { onSnake = true; break; }
        }
        if (!onSnake) { g.food.x = x; g.food.y = y; return; }
    }
}

function stepGame(g) {
    if (!g.alive) return;

    // Commit queued direction (prevent instant reverse into self)
    if (!(g.nextDir.x === -g.dir.x && g.nextDir.y === -g.dir.y)) {
        g.dir.x = g.nextDir.x;
        g.dir.y = g.nextDir.y;
    }

    var head = g.snake[0];
    var nx = head.x + g.dir.x;
    var ny = head.y + g.dir.y;

    // Wall collision
    if (nx < 0 || ny < 0 || nx >= g.grid.cols || ny >= g.grid.rows) {
        die(g);
        return;
    }

    // Self collision (ignore tail if it's moving out of the way and we're not growing)
    var tailIdx = g.snake.length - 1;
    for (var i = 0; i < g.snake.length; i++) {
        if (i === tailIdx && g.growPending === 0) continue;
        if (g.snake[i].x === nx && g.snake[i].y === ny) {
            die(g);
            return;
        }
    }

    // Move: prepend new head
    g.snake.unshift({ x: nx, y: ny });

    // Food?
    if (nx === g.food.x && ny === g.food.y) {
        g.score += 10;
        g.growPending += 1;
        // Speed up: interpolate from TICK_MAX toward TICK_MIN based on length
        var len = g.snake.length;
        var t = Math.min(1, (len - 3) / 40);
        g.stepInterval = TICK_MAX + (TICK_MIN - TICK_MAX) * t;
        Audio.sfxEat();
        placeFood(g);
        updateHUD(g);
        g.flashTimer = 120;
    }

    // Consume grow or pop tail
    if (g.growPending > 0) {
        g.growPending -= 1;
    } else {
        g.snake.pop();
    }
}

function die(g) {
    g.alive = false;
    Audio.sfxDie();
    if (g.score > Storage.highScore) {
        Storage.highScore = g.score;
        Storage.save();
    }
    Screens.switchTo("gameover");
}

// ---------- Rendering ----------
function computeBoard(W, H) {
    var margin = 40;
    var availW = W - margin * 2;
    var availH = H - margin * 2;
    var cell = Math.floor(Math.min(availW / COLS, availH / ROWS));
    if (cell < 6) cell = 6;
    var boardW = cell * COLS;
    var boardH = cell * ROWS;
    var ox = Math.floor((W - boardW) / 2);
    var oy = Math.floor((H - boardH) / 2);
    return { ox: ox, oy: oy, cell: cell, w: boardW, h: boardH };
}

function drawGame(g, W, H) {
    var b = computeBoard(W, H);

    // Board background
    ctx.fillStyle = "#0d1a12";
    ctx.fillRect(b.ox, b.oy, b.w, b.h);

    // Subtle grid
    ctx.strokeStyle = "#142b1e";
    ctx.lineWidth = 1;
    ctx.beginPath();
    for (var c = 1; c < COLS; c++) {
        var x = b.ox + c * b.cell + 0.5;
        ctx.moveTo(x, b.oy);
        ctx.lineTo(x, b.oy + b.h);
    }
    for (var r = 1; r < ROWS; r++) {
        var y = b.oy + r * b.cell + 0.5;
        ctx.moveTo(b.ox, y);
        ctx.lineTo(b.ox + b.w, y);
    }
    ctx.stroke();

    // Board border
    ctx.strokeStyle = "#2a5a3b";
    ctx.lineWidth = 2;
    ctx.strokeRect(b.ox - 1, b.oy - 1, b.w + 2, b.h + 2);

    // Food
    var pulse = 1.0;
    if (g.flashTimer > 0) pulse = 1.0 + 0.15 * (g.flashTimer / 120);
    var pad = Math.max(2, Math.floor(b.cell * 0.15));
    var fx = b.ox + g.food.x * b.cell + pad;
    var fy = b.oy + g.food.y * b.cell + pad;
    var fs = b.cell - pad * 2;
    ctx.fillStyle = "#e74c3c";
    ctx.beginPath();
    var cx = fx + fs / 2, cy = fy + fs / 2;
    ctx.arc(cx, cy, (fs / 2) * pulse, 0, Math.PI * 2);
    ctx.fill();
    // highlight
    ctx.fillStyle = "rgba(255,255,255,0.35)";
    ctx.beginPath();
    ctx.arc(cx - fs * 0.15, cy - fs * 0.15, fs * 0.12, 0, Math.PI * 2);
    ctx.fill();

    // Snake
    for (var i = g.snake.length - 1; i >= 0; i--) {
        var seg = g.snake[i];
        var sx = b.ox + seg.x * b.cell;
        var sy = b.oy + seg.y * b.cell;
        var isHead = (i === 0);
        // Gradient shade: head brightest, body fades slightly with length
        var shade = 1.0 - Math.min(0.4, i / (g.snake.length + 4));
        var rCol = Math.floor(123 * shade);
        var gCol = Math.floor(216 * shade);
        var bCol = Math.floor(143 * shade);
        ctx.fillStyle = "rgb(" + rCol + "," + gCol + "," + bCol + ")";
        var p = Math.max(1, Math.floor(b.cell * 0.08));
        ctx.fillRect(sx + p, sy + p, b.cell - p * 2, b.cell - p * 2);

        if (isHead && g.alive) {
            // Eyes oriented by direction
            var eyeR = Math.max(1, Math.floor(b.cell * 0.08));
            var cxh = sx + b.cell / 2;
            var cyh = sy + b.cell / 2;
            var off = b.cell * 0.22;
            var perpX = -g.dir.y, perpY = g.dir.x;
            var e1x = cxh + g.dir.x * off + perpX * off * 0.5;
            var e1y = cyh + g.dir.y * off + perpY * off * 0.5;
            var e2x = cxh + g.dir.x * off - perpX * off * 0.5;
            var e2y = cyh + g.dir.y * off - perpY * off * 0.5;
            ctx.fillStyle = "#06100a";
            ctx.beginPath(); ctx.arc(e1x, e1y, eyeR, 0, Math.PI * 2); ctx.fill();
            ctx.beginPath(); ctx.arc(e2x, e2y, eyeR, 0, Math.PI * 2); ctx.fill();
        }
    }
}

// ---------- HUD ----------
function updateHUD(g) {
    var s = document.getElementById("hud-score");
    var bst = document.getElementById("hud-best");
    var l = document.getElementById("hud-length");
    if (s) s.textContent = String(g.score);
    if (bst) bst.textContent = String(Storage.highScore);
    if (l) l.textContent = String(g.snake.length);
}

// ---------- Screens ----------
var Screens = {
    current: "title",
    selIndex: {},   // screen -> selected menu index

    init: function() {
        this.selIndex["title"] = 0;
        this.selIndex["gameover"] = 0;
        this.selIndex["pause"] = 0;
        this.selIndex["howtoplay"] = 0;
    },

    getName: function() { return this.current; },

    getMenuItems: function(screenId) {
        var el = document.getElementById("screen-" + screenId);
        if (!el) return [];
        return el.querySelectorAll(".menu-item");
    },

    refreshSelection: function() {
        var items = this.getMenuItems(this.current);
        var idx = this.selIndex[this.current] || 0;
        if (idx >= items.length) idx = 0;
        this.selIndex[this.current] = idx;
        for (var i = 0; i < items.length; i++) {
            if (i === idx) items[i].classList.add("selected");
            else items[i].classList.remove("selected");
        }
    },

    switchTo: function(name) {
        // Hide all screens
        var screens = document.querySelectorAll(".screen");
        for (var i = 0; i < screens.length; i++) {
            screens[i].style.display = "none";
        }

        var overlay = document.getElementById("overlay");
        var hud = document.getElementById("hud");

        this.current = name;

        if (name === "playing") {
            overlay.style.display = "none";
            hud.style.display = "block";
            updateHUD(game);
            return;
        }

        overlay.style.display = "block";
        hud.style.display = (name === "pause" || name === "gameover") ? "block" : "none";

        var screenEl = document.getElementById("screen-" + name);
        if (screenEl) screenEl.style.display = "block";

        if (name === "gameover") {
            var stats = document.getElementById("gameover-stats");
            if (stats) {
                var best = Storage.highScore;
                var newBest = (game && game.score >= best && game.score > 0) ? "  (NEW BEST!)" : "";
                stats.textContent =
                    "Score:  " + (game ? game.score : 0) + newBest + "\n" +
                    "Length: " + (game ? game.snake.length : 0) + "\n" +
                    "Best:   " + best;
            }
            updateHUD(game);
        }

        this.refreshSelection();
    },

    moveSel: function(delta) {
        var items = this.getMenuItems(this.current);
        if (items.length === 0) return;
        var idx = this.selIndex[this.current] || 0;
        idx = (idx + delta + items.length) % items.length;
        this.selIndex[this.current] = idx;
        this.refreshSelection();
        Audio.sfxMenu();
    },

    activate: function() {
        var items = this.getMenuItems(this.current);
        var idx = this.selIndex[this.current] || 0;
        if (idx >= items.length) return;
        var action = items[idx].getAttribute("data-action");
        Audio.sfxSelect();
        this.doAction(action);
    },

    doAction: function(action) {
        switch (action) {
            case "play":
            case "restart":
                game = createGame();
                updateHUD(game);
                this.switchTo("playing");
                break;
            case "resume":
                this.switchTo("playing");
                break;
            case "quit":
                this.switchTo("title");
                break;
            case "howtoplay":
                this.switchTo("howtoplay");
                break;
            case "back":
                this.switchTo("title");
                break;
        }
    },

    keydown: function(key) {
        if (this.current === "playing") {
            // Direction input
            var nd = null;
            if (key === "ArrowUp" || key === "w" || key === "W") nd = { x: 0, y: -1 };
            else if (key === "ArrowDown" || key === "s" || key === "S") nd = { x: 0, y: 1 };
            else if (key === "ArrowLeft" || key === "a" || key === "A") nd = { x: -1, y: 0 };
            else if (key === "ArrowRight" || key === "d" || key === "D") nd = { x: 1, y: 0 };
            if (nd && game) {
                // Disallow direct reverse relative to current committed dir
                if (!(nd.x === -game.dir.x && nd.y === -game.dir.y)) {
                    game.nextDir = nd;
                }
                return;
            }
            if (key === "Escape" || key === "p" || key === "P") {
                this.switchTo("pause");
                return;
            }
            return;
        }

        // Menu screens
        if (key === "ArrowUp" || key === "w" || key === "W") { this.moveSel(-1); return; }
        if (key === "ArrowDown" || key === "s" || key === "S") { this.moveSel(1); return; }
        if (key === "Enter" || key === " ") { this.activate(); return; }
        if (key === "Escape") {
            if (this.current === "pause") this.doAction("resume");
            else if (this.current === "howtoplay") this.doAction("back");
            else if (this.current === "gameover") this.doAction("quit");
            return;
        }
    }
};

// ---------- Update / draw ----------
function update(dt) {
    if (Screens.current !== "playing" || !game || !game.alive) return;
    game.stepTimer += dt;
    while (game.stepTimer >= game.stepInterval) {
        game.stepTimer -= game.stepInterval;
        stepGame(game);
        if (!game.alive) break;
    }
    if (game.flashTimer > 0) {
        game.flashTimer -= dt;
        if (game.flashTimer < 0) game.flashTimer = 0;
    }
}

function draw(W, H) {
    ctx.fillStyle = "#06100a";
    ctx.fillRect(0, 0, W, H);
    if (game) drawGame(game, W, H);
}

// ---------- Main loop ----------
Screens.init();
Screens.switchTo("title");

var lastFrameTime = performance.now();
function gameLoop(ts) {
    requestAnimationFrame(gameLoop);
    var dt = ts - lastFrameTime;
    lastFrameTime = ts;
    if (dt > 100) dt = 100;
    if (dt < 0) dt = 0;

    var W = getW(), H = getH();
    update(dt);
    draw(W, H);
}

// ---------- Events ----------
document.body.addEventListener("keydown", function(e) {
    Screens.keydown(e.key);
});

// Menu item click support
document.addEventListener("click", function(e) {
    var t = e.target;
    if (!t || !t.classList) return;
    if (t.classList.contains("menu-item")) {
        var action = t.getAttribute("data-action");
        if (action) {
            Audio.sfxSelect();
            Screens.doAction(action);
        }
    }
});

requestAnimationFrame(gameLoop);

console.log("Snake loaded!");
})();
