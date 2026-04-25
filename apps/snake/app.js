// app.js — Classic Snake for the bro runtime
(function() {
"use strict";

SFX.init();
const store = Storage.create("snake");
store.load({ highScore: 0 });

Input.init([
    { name: "up",      label: "Up",      defaults: ["w", "ArrowUp"] },
    { name: "down",    label: "Down",    defaults: ["s", "ArrowDown"] },
    { name: "left",    label: "Left",    defaults: ["a", "ArrowLeft"] },
    { name: "right",   label: "Right",   defaults: ["d", "ArrowRight"] },
    { name: "pause",   label: "Pause",   defaults: ["Escape", "p"] },
    { name: "confirm", label: "Confirm", defaults: ["Enter", " "] },
]);
Input.attach(window);

// ---------- SFX ----------
const sfx = {
    eat:    () => SFX.tone(660, 0.08, "square", 0.6),
    die:    () => { SFX.sequence([[300,0.15,"sawtooth",0.5],[200,0.2,"sawtooth",0.5],[120,0.3,"sawtooth",0.5]]); },
    menu:   () => SFX.tone(440, 0.04, "sine", 0.3),
    select: () => SFX.tone(660, 0.07, "square", 0.4),
};

// ---------- Canvas ----------
const canvas = document.getElementById("game");
const ctx = canvas.getContext("2d");
const getW = () => Canvas.w(ctx, 800);
const getH = () => Canvas.h(ctx, 700);

// ---------- Constants ----------
const COLS = 28, ROWS = 22;
const TICK_MIN = 55, TICK_MAX = 140;

// ---------- Game state ----------
let game = null;

function createGame() {
    const g = {
        grid: { cols: COLS, rows: ROWS },
        snake: [], dir: { x: 1, y: 0 }, nextDir: { x: 1, y: 0 },
        food: { x: 0, y: 0 }, growPending: 0, score: 0,
        stepInterval: TICK_MAX, stepTimer: 0,
        alive: true, flashTimer: 0,
    };
    const cx = Math.floor(COLS / 2), cy = Math.floor(ROWS / 2);
    g.snake.push({ x: cx, y: cy });
    g.snake.push({ x: cx - 1, y: cy });
    g.snake.push({ x: cx - 2, y: cy });
    placeFood(g);
    return g;
}

function placeFood(g) {
    for (let tries = 0; tries < 500; tries++) {
        const x = Math.floor(Math.random() * g.grid.cols);
        const y = Math.floor(Math.random() * g.grid.rows);
        let onSnake = false;
        for (const s of g.snake) if (s.x === x && s.y === y) { onSnake = true; break; }
        if (!onSnake) { g.food.x = x; g.food.y = y; return; }
    }
}

function stepGame(g) {
    if (!g.alive) return;
    if (!(g.nextDir.x === -g.dir.x && g.nextDir.y === -g.dir.y)) {
        g.dir.x = g.nextDir.x; g.dir.y = g.nextDir.y;
    }
    const head = g.snake[0];
    const nx = head.x + g.dir.x, ny = head.y + g.dir.y;
    if (nx < 0 || ny < 0 || nx >= g.grid.cols || ny >= g.grid.rows) { die(g); return; }
    const tailIdx = g.snake.length - 1;
    for (let i = 0; i < g.snake.length; i++) {
        if (i === tailIdx && g.growPending === 0) continue;
        if (g.snake[i].x === nx && g.snake[i].y === ny) { die(g); return; }
    }
    g.snake.unshift({ x: nx, y: ny });
    if (nx === g.food.x && ny === g.food.y) {
        g.score += 10;
        g.growPending += 1;
        const t = Math.min(1, (g.snake.length - 3) / 40);
        g.stepInterval = TICK_MAX + (TICK_MIN - TICK_MAX) * t;
        sfx.eat();
        placeFood(g);
        updateHUD(g);
        g.flashTimer = 120;
    }
    if (g.growPending > 0) g.growPending -= 1;
    else g.snake.pop();
}

function die(g) {
    g.alive = false;
    sfx.die();
    if (g.score > store.get("highScore")) {
        store.set("highScore", g.score);
        store.save();
    }
    screens.switchTo("gameover");
}

// ---------- Rendering ----------
function computeBoard(W, H) {
    const margin = 40;
    const availW = W - margin * 2, availH = H - margin * 2;
    let cell = Math.floor(Math.min(availW / COLS, availH / ROWS));
    if (cell < 6) cell = 6;
    const boardW = cell * COLS, boardH = cell * ROWS;
    return {
        ox: Math.floor((W - boardW) / 2),
        oy: Math.floor((H - boardH) / 2),
        cell, w: boardW, h: boardH,
    };
}

function drawGame(g, W, H) {
    const b = computeBoard(W, H);

    ctx.fillStyle = "#0d1a12";
    ctx.fillRect(b.ox, b.oy, b.w, b.h);

    ctx.strokeStyle = "#142b1e";
    ctx.lineWidth = 1;
    ctx.beginPath();
    for (let c = 1; c < COLS; c++) {
        const x = b.ox + c * b.cell + 0.5;
        ctx.moveTo(x, b.oy); ctx.lineTo(x, b.oy + b.h);
    }
    for (let r = 1; r < ROWS; r++) {
        const y = b.oy + r * b.cell + 0.5;
        ctx.moveTo(b.ox, y); ctx.lineTo(b.ox + b.w, y);
    }
    ctx.stroke();

    ctx.strokeStyle = "#2a5a3b";
    ctx.lineWidth = 2;
    ctx.strokeRect(b.ox - 1, b.oy - 1, b.w + 2, b.h + 2);

    const pulse = g.flashTimer > 0 ? 1.0 + 0.15 * (g.flashTimer / 120) : 1.0;
    const pad = Math.max(2, Math.floor(b.cell * 0.15));
    const fx = b.ox + g.food.x * b.cell + pad;
    const fy = b.oy + g.food.y * b.cell + pad;
    const fs = b.cell - pad * 2;
    const cxF = fx + fs / 2, cyF = fy + fs / 2;
    ctx.fillStyle = "#e74c3c";
    ctx.beginPath(); ctx.arc(cxF, cyF, (fs / 2) * pulse, 0, Math.PI * 2); ctx.fill();
    ctx.fillStyle = "rgba(255,255,255,0.35)";
    ctx.beginPath(); ctx.arc(cxF - fs * 0.15, cyF - fs * 0.15, fs * 0.12, 0, Math.PI * 2); ctx.fill();

    for (let i = g.snake.length - 1; i >= 0; i--) {
        const seg = g.snake[i];
        const sx = b.ox + seg.x * b.cell, sy = b.oy + seg.y * b.cell;
        const shade = 1.0 - Math.min(0.4, i / (g.snake.length + 4));
        ctx.fillStyle = `rgb(${Math.floor(123*shade)},${Math.floor(216*shade)},${Math.floor(143*shade)})`;
        const p = Math.max(1, Math.floor(b.cell * 0.08));
        ctx.fillRect(sx + p, sy + p, b.cell - p * 2, b.cell - p * 2);

        if (i === 0 && g.alive) {
            const eyeR = Math.max(1, Math.floor(b.cell * 0.08));
            const cxh = sx + b.cell / 2, cyh = sy + b.cell / 2;
            const off = b.cell * 0.22;
            const perpX = -g.dir.y, perpY = g.dir.x;
            const e1x = cxh + g.dir.x * off + perpX * off * 0.5;
            const e1y = cyh + g.dir.y * off + perpY * off * 0.5;
            const e2x = cxh + g.dir.x * off - perpX * off * 0.5;
            const e2y = cyh + g.dir.y * off - perpY * off * 0.5;
            ctx.fillStyle = "#06100a";
            ctx.beginPath(); ctx.arc(e1x, e1y, eyeR, 0, Math.PI * 2); ctx.fill();
            ctx.beginPath(); ctx.arc(e2x, e2y, eyeR, 0, Math.PI * 2); ctx.fill();
        }
    }
}

// ---------- HUD ----------
function updateHUD(g) {
    const scoreEl = document.getElementById("hud-score");
    const bestEl  = document.getElementById("hud-best");
    const lenEl   = document.getElementById("hud-length");
    if (scoreEl) scoreEl.textContent = String(g ? g.score : 0);
    if (bestEl)  bestEl.textContent  = String(store.get("highScore"));
    if (lenEl)   lenEl.textContent   = String(g ? g.snake.length : 3);
}

// ---------- Screens ----------
const hudEl = document.getElementById("hud");
const overlayEl = document.getElementById("overlay");

const screens = Screens.create({
    overlay: "#overlay",
    itemsSelector: ".menu-items",
    onMenuMove: sfx.menu,
    onMenuSelect: sfx.select,
});

function doAction(action) {
    switch (action) {
        case "play":
        case "restart":
            game = createGame(); updateHUD(game); screens.switchTo("playing"); break;
        case "resume":   screens.switchTo("playing"); break;
        case "quit":     screens.switchTo("title"); break;
        case "howtoplay":screens.switchTo("howtoplay"); break;
        case "back":     screens.switchTo("title"); break;
    }
}

function showOverlay(name, withHud) {
    screens.showOverlay(name);
    hudEl.style.display = withHud ? "block" : "none";
}

screens.define("title", {
    enter: () => showOverlay("title", false),
    keydown: (key) => {
        screens.menuNav("title", key, (idx, el) => doAction(el.getAttribute("data-action")),
            { onBack: () => doAction("quit") });
    },
});
screens.define("howtoplay", {
    enter: () => showOverlay("howtoplay", false),
    keydown: (key) => {
        screens.menuNav("howtoplay", key, (idx, el) => doAction(el.getAttribute("data-action")),
            { onBack: () => doAction("back") });
    },
});
screens.define("playing", {
    enter: () => { overlayEl.style.display = "none"; hudEl.style.display = "block"; updateHUD(game); },
});
screens.define("pause", {
    enter: () => showOverlay("pause", true),
    keydown: (key) => {
        screens.menuNav("pause", key, (idx, el) => doAction(el.getAttribute("data-action")),
            { onBack: () => doAction("resume") });
    },
});
screens.define("gameover", {
    enter: () => {
        showOverlay("gameover", true);
        const stats = document.getElementById("gameover-stats");
        if (stats) {
            const best = store.get("highScore");
            const newBest = (game && game.score >= best && game.score > 0) ? "  (NEW BEST!)" : "";
            stats.textContent =
                "Score:  " + (game ? game.score : 0) + newBest + "\n" +
                "Length: " + (game ? game.snake.length : 0) + "\n" +
                "Best:   " + best;
        }
        updateHUD(game);
    },
    keydown: (key) => {
        screens.menuNav("gameover", key, (idx, el) => doAction(el.getAttribute("data-action")),
            { onBack: () => doAction("quit") });
    },
});

screens.switchTo("title");

// ---------- Input routing ----------
// Rising-edge actions: queue next direction, pause toggle.
Input.onAction((action, phase) => {
    if (phase !== "down") return;
    if (screens.name() === "playing") {
        let nd = null;
        if (action === "up")    nd = { x: 0, y: -1 };
        if (action === "down")  nd = { x: 0, y:  1 };
        if (action === "left")  nd = { x: -1, y: 0 };
        if (action === "right") nd = { x: 1,  y: 0 };
        if (nd && game) {
            if (!(nd.x === -game.dir.x && nd.y === -game.dir.y)) game.nextDir = nd;
            return;
        }
        if (action === "pause") { screens.switchTo("pause"); return; }
        return;
    }

    // Menus: funnel action names back to the screen as raw keys so menuNav
    // (which speaks in DOM key strings) can handle them uniformly.
    if (action === "up")      screens.keydown("ArrowUp");
    else if (action === "down")  screens.keydown("ArrowDown");
    else if (action === "left")  screens.keydown("ArrowLeft");
    else if (action === "right") screens.keydown("ArrowRight");
    else if (action === "confirm") screens.keydown("Enter");
    else if (action === "pause") screens.keydown("Escape");
});

// ---------- Loop ----------
function update(dt) {
    if (screens.name() !== "playing" || !game || !game.alive) return;
    game.stepTimer += dt;
    while (game.stepTimer >= game.stepInterval) {
        game.stepTimer -= game.stepInterval;
        stepGame(game);
        if (!game.alive) break;
    }
    if (game.flashTimer > 0) game.flashTimer = Math.max(0, game.flashTimer - dt);
}

function draw() {
    const W = getW(), H = getH();
    ctx.fillStyle = "#06100a";
    ctx.fillRect(0, 0, W, H);
    if (game) drawGame(game, W, H);
}

GameLoop.create({ tick: update, draw }).start();
})();
