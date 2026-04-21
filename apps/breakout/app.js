// app.js — Breakout game
(function() {
"use strict";

// --- Canvas ---
var canvas = document.getElementById("game");
var ctx = canvas.getContext("2d");
var getW = function() { return ctx.canvasWidth || canvas.width || 800; };
var getH = function() { return ctx.canvasHeight || canvas.height || 700; };

// --- Storage ---
var Storage = {
    highScore: 0,
    load: function() {
        try {
            var v = localStorage.getItem("breakout_highscore");
            if (v) this.highScore = parseInt(v, 10) || 0;
        } catch(e) {}
    },
    save: function() {
        try { localStorage.setItem("breakout_highscore", String(this.highScore)); } catch(e) {}
    }
};
Storage.load();

// --- Audio (optional, graceful fallback) ---
var Audio = {
    ctx: null, sfxBus: -1,
    init: function() {
        try { this.ctx = new AudioContext(); } catch(e) { this.ctx = null; return; }
        try {
            this.sfxBus = this.ctx.createBus();
            this.ctx.setBusGain(this.sfxBus, 0.7);
        } catch(e) { this.sfxBus = -1; }
    },
    tone: function(freq, duration, type, vol) {
        if (!this.ctx) return;
        try {
            var id = this.ctx.createVoice();
            this.ctx.setVoiceWaveform(id, type || "square");
            this.ctx.setVoiceFrequency(id, freq);
            this.ctx.setVoiceGain(id, (vol || 0.6) * 15.0);
            this.ctx.setVoiceAttack(id, 0.003);
            this.ctx.setVoiceDecay(id, duration * 0.8);
            this.ctx.setVoiceSustain(id, 0.0);
            this.ctx.setVoiceRelease(id, 0.02);
            if (this.sfxBus !== -1) this.ctx.setVoiceBus(id, this.sfxBus);
            var t = this.ctx.currentTime;
            this.ctx.startVoice(id, t);
            this.ctx.stopVoice(id, t + duration);
        } catch(e) {}
    },
    paddleHit: function() { this.tone(220, 0.05, "square", 0.5); },
    wallHit:   function() { this.tone(180, 0.04, "square", 0.4); },
    brickHit:  function(row) {
        var f = 400 + row * 60;
        this.tone(f, 0.07, "square", 0.6);
    },
    loseLife:  function() {
        var self = this;
        this.tone(300, 0.15, "sawtooth", 0.5);
        setTimeout(function() { self.tone(200, 0.2, "sawtooth", 0.5); }, 140);
    },
    gameOver:  function() {
        var self = this;
        this.tone(300, 0.2, "sawtooth", 0.5);
        setTimeout(function() { self.tone(250, 0.2, "sawtooth", 0.5); }, 200);
        setTimeout(function() { self.tone(180, 0.4, "sawtooth", 0.5); }, 400);
    },
    levelClear: function() {
        var self = this;
        this.tone(523, 0.1, "square", 0.7);
        setTimeout(function() { self.tone(659, 0.1, "square", 0.7); }, 90);
        setTimeout(function() { self.tone(784, 0.15, "square", 0.8); }, 180);
    },
    menuMove:   function() { this.tone(400, 0.03, "sine", 0.3); },
    menuSelect: function() { this.tone(600, 0.08, "square", 0.4); },
    launch:     function() { this.tone(500, 0.08, "triangle", 0.5); }
};
Audio.init();

// --- Game state ---
var Game = {
    // World dimensions get refreshed from canvas each frame
    W: 800, H: 700,

    // Paddle
    paddle: { x: 350, y: 640, w: 110, h: 14, speed: 620 },

    // Ball
    ball: { x: 400, y: 620, r: 8, vx: 0, vy: 0, stuck: true },

    // Bricks: array of { x,y,w,h,row,color,points,alive }
    bricks: [],
    bricksAlive: 0,

    // Meta
    score: 0,
    lives: 3,
    level: 1,

    // Input
    keyLeft: false, keyRight: false,
    mouseX: -1, mouseControl: false,

    baseBallSpeed: 380,

    reset: function() {
        this.score = 0;
        this.lives = 3;
        this.level = 1;
        this.setupLevel();
    },

    setupLevel: function() {
        this.bricks = [];
        var cols = 11;
        var rows = 6;
        var margin = 40;
        var top = 80;
        var gap = 4;
        var bw = Math.floor((this.W - margin * 2 - (cols - 1) * gap) / cols);
        var bh = 22;
        // 5 distinct color bands, rows map to a band
        var colors = ["#ef5350", "#ff9100", "#ffee58", "#66bb6a", "#42a5f5", "#ab47bc"];
        var pts    = [50,        40,        30,        20,        10,        10];
        for (var r = 0; r < rows; r++) {
            for (var c = 0; c < cols; c++) {
                this.bricks.push({
                    x: margin + c * (bw + gap),
                    y: top + r * (bh + gap),
                    w: bw, h: bh,
                    row: r,
                    color: colors[r],
                    points: pts[r],
                    alive: true
                });
            }
        }
        this.bricksAlive = this.bricks.length;
        this.resetBall();
    },

    resetBall: function() {
        this.paddle.x = (this.W - this.paddle.w) / 2;
        this.paddle.y = this.H - 60;
        this.ball.r = 8;
        this.ball.stuck = true;
        this.ball.vx = 0;
        this.ball.vy = 0;
        this.ball.x = this.paddle.x + this.paddle.w / 2;
        this.ball.y = this.paddle.y - this.ball.r - 1;
    },

    launchBall: function() {
        if (!this.ball.stuck) return;
        this.ball.stuck = false;
        // Slight random angle, always upward
        var angle = (-Math.PI / 2) + (Math.random() - 0.5) * (Math.PI / 4);
        var sp = this.currentSpeed();
        this.ball.vx = Math.cos(angle) * sp;
        this.ball.vy = Math.sin(angle) * sp;
        Audio.launch();
    },

    currentSpeed: function() {
        return this.baseBallSpeed + (this.level - 1) * 40;
    },

    updateBall: function(dt) {
        var b = this.ball, p = this.paddle;
        if (b.stuck) {
            // ball rides the paddle
            b.x = p.x + p.w / 2;
            b.y = p.y - b.r - 1;
            return;
        }

        var dts = dt / 1000;
        // Substep to avoid tunneling at high speeds
        var sp = Math.sqrt(b.vx*b.vx + b.vy*b.vy);
        var maxStep = 6; // pixels per substep
        var steps = Math.max(1, Math.ceil(sp * dts / maxStep));
        var sdt = dts / steps;
        for (var s = 0; s < steps; s++) {
            b.x += b.vx * sdt;
            b.y += b.vy * sdt;

            // Walls
            if (b.x - b.r < 0) { b.x = b.r; b.vx = -b.vx; Audio.wallHit(); }
            else if (b.x + b.r > this.W) { b.x = this.W - b.r; b.vx = -b.vx; Audio.wallHit(); }
            if (b.y - b.r < 0) { b.y = b.r; b.vy = -b.vy; Audio.wallHit(); }

            // Paddle
            if (b.vy > 0
                && b.y + b.r >= p.y
                && b.y - b.r <= p.y + p.h
                && b.x + b.r >= p.x
                && b.x - b.r <= p.x + p.w) {
                // Angle varies by hit position
                var hit = (b.x - (p.x + p.w / 2)) / (p.w / 2); // -1..1
                if (hit < -1) hit = -1; else if (hit > 1) hit = 1;
                var maxAngle = Math.PI * 0.40; // ~72 deg from vertical
                var angle = hit * maxAngle - Math.PI / 2;
                var speed = this.currentSpeed();
                b.vx = Math.cos(angle) * speed;
                b.vy = Math.sin(angle) * speed;
                b.y = p.y - b.r - 1;
                Audio.paddleHit();
            }

            // Bricks
            for (var i = 0; i < this.bricks.length; i++) {
                var br = this.bricks[i];
                if (!br.alive) continue;
                if (b.x + b.r < br.x || b.x - b.r > br.x + br.w) continue;
                if (b.y + b.r < br.y || b.y - b.r > br.y + br.h) continue;

                // Determine collision side using previous position
                var prevX = b.x - b.vx * sdt;
                var prevY = b.y - b.vy * sdt;
                var wasLeft   = prevX + b.r <= br.x;
                var wasRight  = prevX - b.r >= br.x + br.w;
                var wasAbove  = prevY + b.r <= br.y;
                var wasBelow  = prevY - b.r >= br.y + br.h;

                if (wasLeft && b.vx > 0)       { b.vx = -b.vx; b.x = br.x - b.r; }
                else if (wasRight && b.vx < 0) { b.vx = -b.vx; b.x = br.x + br.w + b.r; }
                else if (wasAbove && b.vy > 0) { b.vy = -b.vy; b.y = br.y - b.r; }
                else if (wasBelow && b.vy < 0) { b.vy = -b.vy; b.y = br.y + br.h + b.r; }
                else {
                    // Corner-ish or tunneling: flip vy as a fallback
                    b.vy = -b.vy;
                }

                br.alive = false;
                this.bricksAlive--;
                this.score += br.points;
                Audio.brickHit(br.row);
                if (this.score > Storage.highScore) {
                    Storage.highScore = this.score;
                    Storage.save();
                }
                break; // only one brick per substep
            }

            // Bottom
            if (b.y - b.r > this.H) {
                this.loseLife();
                return;
            }

            if (this.bricksAlive <= 0) {
                this.onLevelClear();
                return;
            }
        }
    },

    loseLife: function() {
        this.lives--;
        Audio.loseLife();
        if (this.lives <= 0) {
            Screens.switchTo("gameover");
            Audio.gameOver();
        } else {
            this.resetBall();
        }
    },

    onLevelClear: function() {
        Audio.levelClear();
        Screens.switchTo("levelclear");
    },

    nextLevel: function() {
        this.level++;
        this.setupLevel();
    },

    updatePaddle: function(dt) {
        var dts = dt / 1000;
        var p = this.paddle;
        if (this.mouseControl && this.mouseX >= 0) {
            // Snap to mouse
            p.x = this.mouseX - p.w / 2;
        } else {
            if (this.keyLeft) p.x -= p.speed * dts;
            if (this.keyRight) p.x += p.speed * dts;
        }
        if (p.x < 0) p.x = 0;
        if (p.x + p.w > this.W) p.x = this.W - p.w;
        p.y = this.H - 60;
    }
};

// --- Screens / menu system ---
var Screens = {
    current: "title",
    selectedIndex: 0,

    init: function() {
        var self = this;
        var allItems = document.querySelectorAll(".menu-item");
        var bindItem = function(el) {
            el.addEventListener("click", function() {
                var items = self.currentItems();
                for (var i = 0; i < items.length; i++) {
                    if (items[i] === el) { self.selectedIndex = i; break; }
                }
                self.highlight();
                self.activate();
            });
            el.addEventListener("mouseover", function() {
                var items = self.currentItems();
                for (var i = 0; i < items.length; i++) {
                    if (items[i] === el && self.selectedIndex !== i) {
                        self.selectedIndex = i;
                        self.highlight();
                        Audio.menuMove();
                        break;
                    }
                }
            });
        };
        for (var i = 0; i < allItems.length; i++) bindItem(allItems[i]);
    },

    currentScreenEl: function() {
        return document.getElementById("screen-" + this.current);
    },

    currentItems: function() {
        var el = this.currentScreenEl();
        if (!el) return [];
        return el.querySelectorAll(".menu-item");
    },

    switchTo: function(name) {
        this.current = name;
        var screens = document.querySelectorAll(".screen");
        for (var i = 0; i < screens.length; i++) screens[i].style.display = "none";
        var overlay = document.getElementById("overlay");
        var hud = document.getElementById("hud");

        if (name === "playing") {
            overlay.style.display = "none";
            hud.style.display = "block";
            return;
        }
        overlay.style.display = "block";
        hud.style.display = (name === "pause" || name === "levelclear") ? "block" : "none";

        if (name === "gameover") {
            var stats = "Final Score: " + Game.score + "\n";
            stats += "Level: " + Game.level + "\n";
            stats += "High Score: " + Storage.highScore;
            var s = document.getElementById("gameover-stats");
            if (s) s.textContent = stats;
        } else if (name === "levelclear") {
            var s2 = document.getElementById("levelclear-stats");
            if (s2) s2.textContent = "Level " + Game.level + " complete!\nScore: " + Game.score;
        }

        var el = document.getElementById("screen-" + name);
        if (el) el.style.display = "block";
        this.selectedIndex = 0;
        this.highlight();
    },

    highlight: function() {
        var items = this.currentItems();
        for (var i = 0; i < items.length; i++) {
            if (i === this.selectedIndex) items[i].classList.add("selected");
            else items[i].classList.remove("selected");
        }
    },

    moveSel: function(delta) {
        var items = this.currentItems();
        if (items.length === 0) return;
        this.selectedIndex = (this.selectedIndex + delta + items.length) % items.length;
        this.highlight();
        Audio.menuMove();
    },

    activate: function() {
        var items = this.currentItems();
        if (items.length === 0) return;
        var el = items[this.selectedIndex];
        var action = el.getAttribute("data-action");
        Audio.menuSelect();
        if (action === "play") {
            Game.reset();
            this.switchTo("playing");
        } else if (action === "resume") {
            this.switchTo("playing");
        } else if (action === "restart") {
            Game.reset();
            this.switchTo("playing");
        } else if (action === "quit") {
            this.switchTo("title");
        } else if (action === "back") {
            this.switchTo("title");
        } else if (action === "howtoplay") {
            this.switchTo("howtoplay");
        } else if (action === "nextlevel") {
            Game.nextLevel();
            this.switchTo("playing");
        }
    },

    keydown: function(key) {
        if (this.current === "playing") {
            if (key === "ArrowLeft") { Game.keyLeft = true; Game.mouseControl = false; }
            else if (key === "ArrowRight") { Game.keyRight = true; Game.mouseControl = false; }
            else if (key === " " || key === "Spacebar") { Game.launchBall(); }
            else if (key === "Escape" || key === "p" || key === "P") { this.switchTo("pause"); }
            return;
        }
        // Menu navigation
        if (key === "ArrowUp") this.moveSel(-1);
        else if (key === "ArrowDown") this.moveSel(1);
        else if (key === "Enter" || key === " ") this.activate();
        else if (key === "Escape") {
            if (this.current === "pause") this.switchTo("playing");
            else if (this.current === "howtoplay") this.switchTo("title");
            else if (this.current !== "title") this.switchTo("title");
        }
    },

    keyup: function(key) {
        if (key === "ArrowLeft") Game.keyLeft = false;
        else if (key === "ArrowRight") Game.keyRight = false;
    }
};

// --- Rendering ---
function draw(W, H) {
    // background
    ctx.fillStyle = "#06060a";
    ctx.fillRect(0, 0, W, H);

    // subtle top line
    ctx.strokeStyle = "#1a1a24";
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(0, 70);
    ctx.lineTo(W, 70);
    ctx.stroke();

    // bricks
    for (var i = 0; i < Game.bricks.length; i++) {
        var b = Game.bricks[i];
        if (!b.alive) continue;
        ctx.fillStyle = b.color;
        ctx.fillRect(b.x, b.y, b.w, b.h);
        // highlight
        ctx.fillStyle = "rgba(255,255,255,0.18)";
        ctx.fillRect(b.x, b.y, b.w, 3);
        ctx.fillStyle = "rgba(0,0,0,0.25)";
        ctx.fillRect(b.x, b.y + b.h - 3, b.w, 3);
    }

    // paddle
    var p = Game.paddle;
    ctx.fillStyle = "#ff9100";
    ctx.fillRect(p.x, p.y, p.w, p.h);
    ctx.fillStyle = "rgba(255,255,255,0.3)";
    ctx.fillRect(p.x, p.y, p.w, 3);

    // ball
    var ball = Game.ball;
    ctx.fillStyle = "#ffffff";
    ctx.beginPath();
    ctx.arc(ball.x, ball.y, ball.r, 0, Math.PI * 2);
    ctx.fill();

    // launch hint
    if (ball.stuck && Screens.current === "playing") {
        ctx.fillStyle = "#aaaaaa";
        ctx.font = "16px Consolas, monospace";
        ctx.textAlign = "center";
        ctx.fillText("Press SPACE to launch", W / 2, H - 20);
    }
}

function updateHUD() {
    var s = document.getElementById("hud-score");      if (s) s.textContent = Game.score;
    var h = document.getElementById("hud-high");       if (h) h.textContent = Storage.highScore;
    var l = document.getElementById("hud-level");      if (l) l.textContent = Game.level;
    var lv = document.getElementById("hud-lives");     if (lv) lv.textContent = Game.lives;
}

// --- Input ---
document.body.addEventListener("keydown", function(e) {
    if (e.repeat) {
        if (Screens.current === "playing") return;
    }
    Screens.keydown(e.key);
});
document.body.addEventListener("keyup", function(e) {
    Screens.keyup(e.key);
});

canvas.addEventListener("mousemove", function(e) {
    // e.offsetX may not exist; compute from clientX and bounding rect
    var rect = canvas.getBoundingClientRect ? canvas.getBoundingClientRect() : null;
    var x;
    if (rect) {
        var scaleX = (ctx.canvasWidth || canvas.width || 800) / (rect.width || 800);
        x = (e.clientX - rect.left) * scaleX;
    } else if (typeof e.offsetX === "number") {
        x = e.offsetX;
    } else {
        x = e.clientX;
    }
    Game.mouseX = x;
    if (Screens.current === "playing") Game.mouseControl = true;
});

canvas.addEventListener("mousedown", function(e) {
    if (Screens.current === "playing") Game.launchBall();
});

// --- Init ---
Screens.init();
Screens.switchTo("title");

// --- Game loop ---
var lastFrameTime = performance.now();
function gameLoop(timestamp) {
    requestAnimationFrame(gameLoop);
    var dt = timestamp - lastFrameTime;
    lastFrameTime = timestamp;
    if (dt > 100) dt = 100;
    if (dt < 0) dt = 0;

    var W = getW(), H = getH();
    Game.W = W;
    Game.H = H;

    if (Screens.current === "playing") {
        Game.updatePaddle(dt);
        Game.updateBall(dt);
        updateHUD();
    } else if (Screens.current === "pause" || Screens.current === "levelclear") {
        updateHUD();
    }

    ctx.clearRect(0, 0, W, H);
    draw(W, H);
}
requestAnimationFrame(gameLoop);

console.log("Breakout loaded!");
})();
