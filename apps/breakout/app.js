// app.js — Breakout game
(function() {
"use strict";

// --- Canvas ---
var canvas = document.getElementById("game");
var ctx = canvas.getContext("2d");
var getW = function() { return Canvas.w(ctx, 800); };
var getH = function() { return Canvas.h(ctx, 700); };

// --- Storage / Audio (library-backed) ---
var _store = Storage.create("breakout");
_store.load({ highScore: 0 });
var HighScore = {
    get value() { return _store.get("highScore") || 0; },
    bump: function(score) {
        if (score > (_store.get("highScore") || 0)) {
            _store.set("highScore", score); _store.save();
        }
    },
};

SFX.init();
var Audio = {
    paddleHit: function() { SFX.tone(220, 0.05, "square", 0.5); },
    wallHit:   function() { SFX.tone(180, 0.04, "square", 0.4); },
    brickHit:  function(row) { SFX.tone(400 + row * 60, 0.07, "square", 0.6); },
    loseLife:  function() { SFX.sequence([[300,0.15,"sawtooth",0.5],[200,0.2,"sawtooth",0.5]]); },
    gameOver:  function() { SFX.sequence([[300,0.2,"sawtooth",0.5],[250,0.2,"sawtooth",0.5],[180,0.4,"sawtooth",0.5]]); },
    levelClear:function() { SFX.sequence([[523,0.1,"square",0.7],[659,0.1,"square",0.7],[784,0.15,"square",0.8]]); },
    menuMove:   function() { SFX.tone(400, 0.03, "sine", 0.3); },
    menuSelect: function() { SFX.tone(600, 0.08, "square", 0.4); },
    launch:     function() { SFX.tone(500, 0.08, "triangle", 0.5); },
};

// --- Input (lib/input + bro.settings) ---
Input.init([
    { name: "left",    label: "Paddle Left",  defaults: ["a", "ArrowLeft"] },
    { name: "right",   label: "Paddle Right", defaults: ["d", "ArrowRight"] },
    { name: "primary", label: "Launch/Fire",  defaults: [" ", "Mouse0"] },
    { name: "up",      label: "Menu Up",      defaults: ["ArrowUp"] },
    { name: "down",    label: "Menu Down",    defaults: ["ArrowDown"] },
    { name: "confirm", label: "Confirm",      defaults: ["Enter"] },
    { name: "pause",   label: "Pause",        defaults: ["Escape", "p"] },
]);
Input.attach(window);

// --- Game state ---
var Game = {
    W: 800, H: 700,
    paddle: { x: 350, y: 640, w: 110, h: 14, speed: 620 },
    ball:   { x: 400, y: 620, r: 8, vx: 0, vy: 0, stuck: true },
    bricks: [], bricksAlive: 0,
    score: 0, lives: 3, level: 1,
    mouseX: -1, mouseControl: false,
    baseBallSpeed: 380,

    reset: function() { this.score = 0; this.lives = 3; this.level = 1; this.setupLevel(); },

    setupLevel: function() {
        this.bricks = [];
        var cols = 11, rows = 6, margin = 40, top = 80, gap = 4;
        var bw = Math.floor((this.W - margin * 2 - (cols - 1) * gap) / cols);
        var bh = 22;
        var colors = ["#ef5350", "#ff9100", "#ffee58", "#66bb6a", "#42a5f5", "#ab47bc"];
        var pts    = [50, 40, 30, 20, 10, 10];
        for (var r = 0; r < rows; r++) {
            for (var c = 0; c < cols; c++) {
                this.bricks.push({
                    x: margin + c * (bw + gap), y: top + r * (bh + gap),
                    w: bw, h: bh, row: r, color: colors[r], points: pts[r], alive: true,
                });
            }
        }
        this.bricksAlive = this.bricks.length;
        this.resetBall();
    },

    resetBall: function() {
        this.paddle.x = (this.W - this.paddle.w) / 2;
        this.paddle.y = this.H - 60;
        this.ball.r = 8; this.ball.stuck = true; this.ball.vx = 0; this.ball.vy = 0;
        this.ball.x = this.paddle.x + this.paddle.w / 2;
        this.ball.y = this.paddle.y - this.ball.r - 1;
    },

    launchBall: function() {
        if (!this.ball.stuck) return;
        this.ball.stuck = false;
        var angle = (-Math.PI / 2) + (Math.random() - 0.5) * (Math.PI / 4);
        var sp = this.currentSpeed();
        this.ball.vx = Math.cos(angle) * sp;
        this.ball.vy = Math.sin(angle) * sp;
        Audio.launch();
    },

    currentSpeed: function() { return this.baseBallSpeed + (this.level - 1) * 40; },

    updateBall: function(dt) {
        var b = this.ball, p = this.paddle;
        if (b.stuck) { b.x = p.x + p.w / 2; b.y = p.y - b.r - 1; return; }
        var dts = dt / 1000;
        var sp = Math.sqrt(b.vx*b.vx + b.vy*b.vy);
        var maxStep = 6;
        var steps = Math.max(1, Math.ceil(sp * dts / maxStep));
        var sdt = dts / steps;
        for (var s = 0; s < steps; s++) {
            b.x += b.vx * sdt; b.y += b.vy * sdt;

            if (b.x - b.r < 0) { b.x = b.r; b.vx = -b.vx; Audio.wallHit(); }
            else if (b.x + b.r > this.W) { b.x = this.W - b.r; b.vx = -b.vx; Audio.wallHit(); }
            if (b.y - b.r < 0) { b.y = b.r; b.vy = -b.vy; Audio.wallHit(); }

            if (b.vy > 0 && b.y + b.r >= p.y && b.y - b.r <= p.y + p.h
                && b.x + b.r >= p.x && b.x - b.r <= p.x + p.w) {
                var hit = (b.x - (p.x + p.w / 2)) / (p.w / 2);
                if (hit < -1) hit = -1; else if (hit > 1) hit = 1;
                var maxAngle = Math.PI * 0.40;
                var angle = hit * maxAngle - Math.PI / 2;
                var speed = this.currentSpeed();
                b.vx = Math.cos(angle) * speed; b.vy = Math.sin(angle) * speed;
                b.y = p.y - b.r - 1;
                Audio.paddleHit();
            }

            for (var i = 0; i < this.bricks.length; i++) {
                var br = this.bricks[i];
                if (!br.alive) continue;
                if (b.x + b.r < br.x || b.x - b.r > br.x + br.w) continue;
                if (b.y + b.r < br.y || b.y - b.r > br.y + br.h) continue;
                var prevX = b.x - b.vx * sdt, prevY = b.y - b.vy * sdt;
                var wasLeft  = prevX + b.r <= br.x;
                var wasRight = prevX - b.r >= br.x + br.w;
                var wasAbove = prevY + b.r <= br.y;
                var wasBelow = prevY - b.r >= br.y + br.h;
                if (wasLeft  && b.vx > 0) { b.vx = -b.vx; b.x = br.x - b.r; }
                else if (wasRight && b.vx < 0) { b.vx = -b.vx; b.x = br.x + br.w + b.r; }
                else if (wasAbove && b.vy > 0) { b.vy = -b.vy; b.y = br.y - b.r; }
                else if (wasBelow && b.vy < 0) { b.vy = -b.vy; b.y = br.y + br.h + b.r; }
                else { b.vy = -b.vy; }
                br.alive = false;
                this.bricksAlive--;
                this.score += br.points;
                Audio.brickHit(br.row);
                HighScore.bump(this.score);
                break;
            }

            if (b.y - b.r > this.H) { this.loseLife(); return; }
            if (this.bricksAlive <= 0) { this.onLevelClear(); return; }
        }
    },

    loseLife: function() {
        this.lives--;
        Audio.loseLife();
        if (this.lives <= 0) { Screens.switchTo("gameover"); Audio.gameOver(); }
        else this.resetBall();
    },

    onLevelClear: function() { Audio.levelClear(); Screens.switchTo("levelclear"); },
    nextLevel: function() { this.level++; this.setupLevel(); },

    updatePaddle: function(dt) {
        var dts = dt / 1000;
        var p = this.paddle;
        if (this.mouseControl && this.mouseX >= 0) {
            p.x = this.mouseX - p.w / 2;
        } else {
            if (Input.down("left"))  p.x -= p.speed * dts;
            if (Input.down("right")) p.x += p.speed * dts;
        }
        if (p.x < 0) p.x = 0;
        if (p.x + p.w > this.W) p.x = this.W - p.w;
        p.y = this.H - 60;
    },
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

    currentScreenEl: function() { return document.getElementById("screen-" + this.current); },

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
            stats += "High Score: " + HighScore.value;
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
            items[i].classList.toggle("selected", i === this.selectedIndex);
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
        if (action === "play" || action === "restart") { Game.reset(); this.switchTo("playing"); }
        else if (action === "resume") this.switchTo("playing");
        else if (action === "quit" || action === "back") this.switchTo("title");
        else if (action === "howtoplay") this.switchTo("howtoplay");
        else if (action === "nextlevel") { Game.nextLevel(); this.switchTo("playing"); }
    },
};

// --- Action routing ---
Input.onAction(function(action, phase) {
    if (phase !== "down" || !action) return;
    if (Screens.current === "playing") {
        if (action === "primary") Game.launchBall();
        else if (action === "pause") Screens.switchTo("pause");
        // left/right movement sampled inside updatePaddle.
        if (action === "left" || action === "right") Game.mouseControl = false;
        return;
    }
    if (action === "up")   Screens.moveSel(-1);
    else if (action === "down") Screens.moveSel(1);
    else if (action === "confirm" || action === "primary") Screens.activate();
    else if (action === "pause") {
        if (Screens.current === "pause") Screens.switchTo("playing");
        else if (Screens.current !== "title") Screens.switchTo("title");
    }
});

// Mouse control: move paddle with pointer, click to launch (primary action
// already handles this via Mouse0 → primary, but we need to register that
// mouse movement should drive paddle position).
canvas.addEventListener("mousemove", function(e) {
    var rect = canvas.getBoundingClientRect ? canvas.getBoundingClientRect() : null;
    var x;
    if (rect) {
        var scaleX = Canvas.w(ctx, 800) / (rect.width || 800);
        x = (e.clientX - rect.left) * scaleX;
    } else if (typeof e.offsetX === "number") x = e.offsetX;
    else x = e.clientX;
    Game.mouseX = x;
    if (Screens.current === "playing") Game.mouseControl = true;
});

// --- Rendering ---
function draw(W, H) {
    ctx.fillStyle = "#06060a"; ctx.fillRect(0, 0, W, H);
    ctx.strokeStyle = "#1a1a24"; ctx.lineWidth = 1;
    ctx.beginPath(); ctx.moveTo(0, 70); ctx.lineTo(W, 70); ctx.stroke();

    for (var i = 0; i < Game.bricks.length; i++) {
        var b = Game.bricks[i];
        if (!b.alive) continue;
        ctx.fillStyle = b.color; ctx.fillRect(b.x, b.y, b.w, b.h);
        ctx.fillStyle = "rgba(255,255,255,0.18)"; ctx.fillRect(b.x, b.y, b.w, 3);
        ctx.fillStyle = "rgba(0,0,0,0.25)"; ctx.fillRect(b.x, b.y + b.h - 3, b.w, 3);
    }

    var p = Game.paddle;
    ctx.fillStyle = "#ff9100"; ctx.fillRect(p.x, p.y, p.w, p.h);
    ctx.fillStyle = "rgba(255,255,255,0.3)"; ctx.fillRect(p.x, p.y, p.w, 3);

    var ball = Game.ball;
    ctx.fillStyle = "#ffffff";
    ctx.beginPath(); ctx.arc(ball.x, ball.y, ball.r, 0, Math.PI * 2); ctx.fill();

    if (ball.stuck && Screens.current === "playing") {
        ctx.fillStyle = "#aaaaaa";
        ctx.font = "16px Consolas, monospace";
        ctx.textAlign = "center";
        ctx.fillText("Click or press SPACE to launch", W / 2, H - 20);
    }
}

function updateHUD() {
    var s = document.getElementById("hud-score");  if (s) s.textContent = Game.score;
    var h = document.getElementById("hud-high");   if (h) h.textContent = HighScore.value;
    var l = document.getElementById("hud-level");  if (l) l.textContent = Game.level;
    var lv = document.getElementById("hud-lives"); if (lv) lv.textContent = Game.lives;
}

// --- Init + loop ---
Screens.init();
Screens.switchTo("title");

GameLoop.create({
    tick: function(dt) {
        var W = getW(), H = getH();
        Game.W = W; Game.H = H;
        if (Screens.current === "playing") {
            Game.updatePaddle(dt);
            Game.updateBall(dt);
            updateHUD();
        } else if (Screens.current === "pause" || Screens.current === "levelclear") {
            updateHUD();
        }
    },
    draw: function() {
        var W = getW(), H = getH();
        ctx.clearRect(0, 0, W, H);
        draw(W, H);
    },
}).start();

console.log("Breakout loaded!");
})();
