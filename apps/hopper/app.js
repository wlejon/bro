// app.js — Hopper: Frogger-style lane-crosser
(function() {
"use strict";

// =========================================================================
// Constants / layout
// =========================================================================
var COLS = 13;
var ROWS = 14;
var TILE = 56; // 13*56 = 728 wide; we center the playfield
var GRID_W = COLS * TILE;
var GRID_H = ROWS * TILE;

// Row layout (top to bottom):
// 0: goals (lily pads)
// 1-5: river (logs drifting)
// 6: median (safe grass)
// 7-11: road (cars)
// 12: start safe strip
// 13: bottom safe strip (player spawn)
var ROW_GOAL = 0;
var ROW_RIVER_START = 1;
var ROW_RIVER_END = 5;
var ROW_MEDIAN = 6;
var ROW_ROAD_START = 7;
var ROW_ROAD_END = 11;
var ROW_SAFE1 = 12;
var ROW_START = 13;

var GOAL_COLS = [1, 4, 6, 8, 11];
var PAD_RADIUS_COLS = 1; // half-width in columns of pad catchment (pad at col c catches c-0.5..c+0.5)

// Game timer
var ROUND_TIME_MS = 60 * 1000;

// =========================================================================
// Audio (broaudio)
// =========================================================================
var Audio = {
    init:   function() { SFX.init(); },
    hop:    function() { SFX.tone(520, 0.06, "square", 0.5); },
    squish: function() { SFX.sequence([[180,0.2,"sawtooth",0.7],[100,0.3,"sawtooth",0.7]]); },
    drown:  function() { SFX.sequence([[300,0.15,"triangle",0.6],[150,0.3,"triangle",0.6]]); },
    pad:    function() { SFX.sequence([[660,0.1,"square",0.7],[880,0.15,"square",0.8]]); },
    win:    function() { SFX.sequence([[523,0.1,"square",0.7],[659,0.1,"square",0.7],[784,0.1,"square",0.7],[1047,0.2,"square",0.8]]); },
    menu:   function() { SFX.tone(400, 0.03, "sine", 0.3); },
    select: function() { SFX.tone(600, 0.08, "square", 0.4); }
};

// =========================================================================
// Storage (namespaced localStorage via lib/storage)
// =========================================================================
var _hopperStore = Storage.create("hopper");
var Store = {
    get highScore() { return _hopperStore.get("highScore") || 0; },
    set highScore(v) { _hopperStore.set("highScore", v); },
    load: function() { _hopperStore.load({ highScore: 0 }); },
    save: function() { _hopperStore.save(); },
};

// =========================================================================
// Game state
// =========================================================================
var Game = {
    // Playfield origin (centered)
    ox: 0, oy: 0,

    // Player (x,y in tile coords; player.y is always an integer when at rest)
    player: { col: 6, row: ROW_START, px: 0, py: 0, onLog: null, anim: 0 },

    lanes: [],   // per row: { dir, speed, entities: [{x,width,type}] }
    pads: [],    // [{col, filled}]

    lives: 3,
    score: 0,
    padsFilled: 0,
    timeLeft: ROUND_TIME_MS,
    level: 1,
    maxRowReached: ROW_START,

    // Overlay effects
    deathTimer: 0,
    deathKind: "",    // "squish" / "drown" / "timeout"
    respawnLock: 0,

    reset: function() {
        this.lives = 3;
        this.score = 0;
        this.level = 1;
        this.padsFilled = 0;
        this.pads = [];
        for (var i = 0; i < GOAL_COLS.length; i++) {
            this.pads.push({ col: GOAL_COLS[i], filled: false });
        }
        this.buildLanes();
        this.respawnPlayer(true);
    },

    newRound: function() {
        this.level++;
        this.padsFilled = 0;
        this.pads = [];
        for (var i = 0; i < GOAL_COLS.length; i++) {
            this.pads.push({ col: GOAL_COLS[i], filled: false });
        }
        this.buildLanes();
        this.respawnPlayer(true);
    },

    respawnPlayer: function(fullReset) {
        this.player.col = 6;
        this.player.row = ROW_START;
        this.player.onLog = null;
        this.player.anim = 0;
        this.maxRowReached = ROW_START;
        if (fullReset) this.timeLeft = ROUND_TIME_MS;
        this.deathTimer = 0;
        this.respawnLock = 200; // brief grace period
    },

    buildLanes: function() {
        this.lanes = new Array(ROWS);
        var lvlMult = 1 + (this.level - 1) * 0.15;

        // Road lanes (rows 7..11) - cars
        // speeds in tiles/sec
        var roadConfigs = [
            { dir: -1, speed: 2.5, gap: 4, width: 1 },   // row 7: slow car right-to-left
            { dir: 1,  speed: 3.5, gap: 3, width: 2 },   // row 8: truck
            { dir: -1, speed: 4.5, gap: 5, width: 1 },   // row 9: fast car
            { dir: 1,  speed: 2.0, gap: 3, width: 1 },   // row 10: slow car
            { dir: -1, speed: 5.5, gap: 6, width: 1 }    // row 11: very fast
        ];
        for (var r = ROW_ROAD_START; r <= ROW_ROAD_END; r++) {
            var cfg = roadConfigs[r - ROW_ROAD_START];
            this.lanes[r] = this.makeLane("car", cfg.dir, cfg.speed * lvlMult, cfg.gap, cfg.width);
        }

        // River lanes (rows 1..5) - logs
        var riverConfigs = [
            { dir: 1,  speed: 1.8, gap: 3, width: 3 },
            { dir: -1, speed: 2.5, gap: 2, width: 2 },
            { dir: 1,  speed: 1.5, gap: 4, width: 4 },
            { dir: -1, speed: 3.0, gap: 3, width: 2 },
            { dir: 1,  speed: 2.2, gap: 3, width: 3 }
        ];
        for (var r2 = ROW_RIVER_START; r2 <= ROW_RIVER_END; r2++) {
            var cfg2 = riverConfigs[r2 - ROW_RIVER_START];
            this.lanes[r2] = this.makeLane("log", cfg2.dir, cfg2.speed * lvlMult, cfg2.gap, cfg2.width);
        }
    },

    makeLane: function(type, dir, speed, gap, width) {
        var entities = [];
        // Fill the lane with entities spaced by gap
        var total = width + gap;
        var startOffset = Math.random() * total;
        // Place entities from -width to COLS+width
        for (var x = -width - 2; x < COLS + width + 2; x += total) {
            entities.push({ x: x + startOffset, width: width, type: type });
        }
        return { dir: dir, speed: speed, entities: entities, type: type, spacing: total, width: width };
    }
};

// =========================================================================
// Canvas
// =========================================================================
var canvas = document.getElementById("game");
var ctx = canvas.getContext("2d");
function getW() { return Canvas.w(ctx, 800); }
function getH() { return Canvas.h(ctx, 800); }

function recalcLayout(W, H) {
    Game.ox = Math.floor((W - GRID_W) / 2);
    Game.oy = Math.floor((H - GRID_H) / 2);
    if (Game.oy < 60) Game.oy = 60;
}

// =========================================================================
// Update
// =========================================================================
function update(dt) {
    if (Game.respawnLock > 0) Game.respawnLock -= dt;

    if (Game.deathTimer > 0) {
        Game.deathTimer -= dt;
        if (Game.deathTimer <= 0) {
            if (Game.lives <= 0) {
                Screens.switchTo("gameover");
                return;
            }
            Game.respawnPlayer(false);
        }
        // still animate background
        updateLanes(dt);
        return;
    }

    // Timer
    Game.timeLeft -= dt;
    if (Game.timeLeft <= 0) {
        Game.timeLeft = 0;
        onDeath("timeout");
        return;
    }

    updateLanes(dt);

    // If player is on a log, drift with it
    if (Game.player.onLog) {
        var log = Game.player.onLog;
        var lane = Game.lanes[Game.player.row];
        if (lane) {
            // Keep the player aligned to the log's drift
            Game.player.col += lane.dir * lane.speed * dt / 1000;
            // If the log moved away from the player column-wise, drop off check below
        }
    }

    // Check collisions/placement based on current row
    var row = Game.player.row;
    if (row >= ROW_ROAD_START && row <= ROW_ROAD_END) {
        // road: check car collisions
        var lane = Game.lanes[row];
        for (var i = 0; i < lane.entities.length; i++) {
            var e = lane.entities[i];
            if (Game.player.col + 0.5 > e.x + 0.05 && Game.player.col + 0.5 < e.x + e.width - 0.05) {
                onDeath("squish");
                return;
            }
        }
    } else if (row >= ROW_RIVER_START && row <= ROW_RIVER_END) {
        // river: must be on a log
        var lane2 = Game.lanes[row];
        var onSomething = null;
        for (var j = 0; j < lane2.entities.length; j++) {
            var e2 = lane2.entities[j];
            if (Game.player.col + 0.5 >= e2.x && Game.player.col + 0.5 <= e2.x + e2.width) {
                onSomething = e2;
                break;
            }
        }
        Game.player.onLog = onSomething;
        if (!onSomething) {
            onDeath("drown");
            return;
        }
        // If player drifts off the sides, drown
        if (Game.player.col < -0.5 || Game.player.col > COLS - 0.5) {
            onDeath("drown");
            return;
        }
    } else {
        Game.player.onLog = null;
    }

    // Goal row handling (if player reaches top)
    if (row === ROW_GOAL) {
        // Player steps directly onto a pad or dies
        handleGoalReached();
    }

    updateHUD();
}

function updateLanes(dt) {
    for (var r = 0; r < ROWS; r++) {
        var lane = Game.lanes[r];
        if (!lane) continue;
        var dx = lane.dir * lane.speed * dt / 1000;
        for (var i = 0; i < lane.entities.length; i++) {
            lane.entities[i].x += dx;
        }
        // Recycle entities that have left the playfield
        var spacing = lane.spacing;
        for (var j = 0; j < lane.entities.length; j++) {
            var e = lane.entities[j];
            if (lane.dir > 0 && e.x > COLS + 2) {
                // find leftmost entity; place this one behind it
                var minX = Infinity;
                for (var k = 0; k < lane.entities.length; k++) if (lane.entities[k].x < minX) minX = lane.entities[k].x;
                e.x = minX - spacing;
            } else if (lane.dir < 0 && e.x + e.width < -2) {
                var maxX = -Infinity;
                for (var k2 = 0; k2 < lane.entities.length; k2++) if (lane.entities[k2].x > maxX) maxX = lane.entities[k2].x;
                e.x = maxX + spacing;
            }
        }
    }
}

function handleGoalReached() {
    // Find nearest pad
    var c = Game.player.col + 0.5;
    var bestIdx = -1;
    var bestDist = 999;
    for (var i = 0; i < Game.pads.length; i++) {
        var d = Math.abs((Game.pads[i].col + 0.5) - c);
        if (d < bestDist) { bestDist = d; bestIdx = i; }
    }
    if (bestIdx >= 0 && bestDist < 0.7 && !Game.pads[bestIdx].filled) {
        // Snap player to the pad center
        Game.pads[bestIdx].filled = true;
        Game.padsFilled++;
        // Score: pad bonus + time bonus fraction
        var timeBonus = Math.floor(Game.timeLeft / 100);
        Game.score += 50 + timeBonus;
        if (Game.score > Store.highScore) {
            Store.highScore = Game.score;
            Store.save();
        }
        Audio.pad();
        if (Game.padsFilled >= Game.pads.length) {
            // Round complete
            Audio.win();
            Game.score += 500; // round bonus
            if (Game.score > Store.highScore) {
                Store.highScore = Game.score;
                Store.save();
            }
            showCenterText("ROUND CLEAR!", 1500);
            Game.respawnPlayer(false);
            Game.respawnLock = 1500;
            setTimeout(function() { Game.newRound(); }, 1500);
        } else {
            Game.respawnPlayer(false);
            Game.timeLeft = ROUND_TIME_MS; // refresh
        }
    } else {
        // Hit the hedge / missed a pad
        onDeath("squish");
    }
}

function onDeath(kind) {
    if (Game.deathTimer > 0) return;
    Game.lives--;
    Game.deathTimer = 1000;
    Game.deathKind = kind;
    if (kind === "drown") Audio.drown();
    else Audio.squish();
    showCenterText(kind === "drown" ? "SPLASH!" : (kind === "timeout" ? "TIME UP!" : "SQUISH!"), 900);
}

function showCenterText(txt, ms) {
    var el = document.getElementById("center-text");
    if (!el) return;
    el.textContent = txt;
    el.style.display = "block";
    setTimeout(function() { el.style.display = "none"; }, ms);
}

function updateHUD() {
    var el;
    el = document.getElementById("hud-score"); if (el) el.textContent = String(Game.score);
    el = document.getElementById("hud-hi"); if (el) el.textContent = String(Store.highScore);
    el = document.getElementById("hud-lives"); if (el) el.textContent = String(Game.lives);
    el = document.getElementById("hud-time"); if (el) el.textContent = String(Math.ceil(Game.timeLeft / 1000));
    el = document.getElementById("hud-pads"); if (el) el.textContent = Game.padsFilled + "/" + Game.pads.length;
}

// =========================================================================
// Input
// =========================================================================
function hop(dx, dy) {
    if (Game.deathTimer > 0 || Game.respawnLock > 0) return;
    var nc = Math.round(Game.player.col) + dx;
    var nr = Game.player.row + dy;
    if (nr < 0 || nr > ROW_START) return;
    if (nc < 0) nc = 0;
    if (nc > COLS - 1) nc = COLS - 1;

    Game.player.col = nc;
    Game.player.row = nr;
    Game.player.onLog = null;
    Audio.hop();

    if (dy < 0 && nr < Game.maxRowReached) {
        Game.maxRowReached = nr;
        Game.score += 10;
    }
    if (Game.score > Store.highScore) {
        Store.highScore = Game.score;
        Store.save();
    }
}

// =========================================================================
// Drawing
// =========================================================================
function draw(W, H) {
    ctx.fillStyle = "#06060a";
    ctx.fillRect(0, 0, W, H);

    var ox = Game.ox;
    var oy = Game.oy;

    // Rows
    for (var r = 0; r < ROWS; r++) {
        var y = oy + r * TILE;
        var color = "#2e7d32"; // grass
        if (r >= ROW_RIVER_START && r <= ROW_RIVER_END) color = "#1565c0"; // water
        else if (r === ROW_MEDIAN) color = "#388e3c"; // median grass
        else if (r >= ROW_ROAD_START && r <= ROW_ROAD_END) color = "#2b2b2b"; // road
        else if (r === ROW_GOAL) color = "#1b3a1b"; // hedge
        ctx.fillStyle = color;
        ctx.fillRect(ox, y, GRID_W, TILE);

        // Road lane stripes
        if (r >= ROW_ROAD_START && r <= ROW_ROAD_END && r < ROW_ROAD_END) {
            ctx.fillStyle = "#f9ca24";
            for (var sx = ox; sx < ox + GRID_W; sx += 20) {
                ctx.fillRect(sx, y + TILE - 2, 10, 4);
            }
        }

        // Water sparkle lines
        if (r >= ROW_RIVER_START && r <= ROW_RIVER_END) {
            ctx.fillStyle = "rgba(255,255,255,0.08)";
            for (var s = 0; s < 6; s++) {
                var sx = ox + ((s * 137 + r * 53) % GRID_W);
                ctx.fillRect(sx, y + (r * 7) % TILE, 20, 2);
            }
        }
    }

    // Goal pads
    for (var i = 0; i < Game.pads.length; i++) {
        var pad = Game.pads[i];
        var px = ox + pad.col * TILE;
        var py = oy + ROW_GOAL * TILE;
        ctx.fillStyle = "#4caf50";
        ctx.beginPath();
        ctx.arc(px + TILE / 2, py + TILE / 2, TILE * 0.4, 0, Math.PI * 2);
        ctx.fill();
        if (pad.filled) {
            // Frog on the pad
            drawFrog(px + TILE / 2, py + TILE / 2, TILE * 0.65, "#689f38");
        }
    }

    // Lanes (cars and logs)
    for (var r2 = 0; r2 < ROWS; r2++) {
        var lane = Game.lanes[r2];
        if (!lane) continue;
        var ly = oy + r2 * TILE;
        for (var j = 0; j < lane.entities.length; j++) {
            var e = lane.entities[j];
            var ex = ox + e.x * TILE;
            var ew = e.width * TILE;
            if (e.type === "car") {
                // Vary color by width
                ctx.fillStyle = e.width >= 2 ? "#c62828" : "#ef6c00";
                ctx.fillRect(ex + 4, ly + 6, ew - 8, TILE - 12);
                // wheels
                ctx.fillStyle = "#111";
                ctx.fillRect(ex + 6, ly + TILE - 10, 10, 6);
                ctx.fillRect(ex + ew - 16, ly + TILE - 10, 10, 6);
                // windshield
                ctx.fillStyle = "rgba(255,255,255,0.25)";
                if (lane.dir > 0) ctx.fillRect(ex + ew - 20, ly + 12, 10, TILE - 24);
                else ctx.fillRect(ex + 10, ly + 12, 10, TILE - 24);
            } else if (e.type === "log") {
                ctx.fillStyle = "#6d4c41";
                ctx.fillRect(ex, ly + 8, ew, TILE - 16);
                ctx.fillStyle = "#4e342e";
                ctx.fillRect(ex, ly + 8, ew, 4);
                ctx.fillRect(ex, ly + TILE - 12, ew, 4);
                // rings
                ctx.fillStyle = "#3e2723";
                ctx.fillRect(ex + 4, ly + 14, 2, TILE - 28);
                ctx.fillRect(ex + ew - 6, ly + 14, 2, TILE - 28);
            }
        }
    }

    // Player
    if (Game.deathTimer <= 0 || (Math.floor(Game.deathTimer / 100) % 2 === 0)) {
        var pcx = ox + (Game.player.col + 0.5) * TILE;
        var pcy = oy + (Game.player.row + 0.5) * TILE;
        drawFrog(pcx, pcy, TILE * 0.75, "#8bc34a");
    }

    // Playfield border
    ctx.strokeStyle = "#000";
    ctx.lineWidth = 2;
    ctx.strokeRect(ox - 1, oy - 1, GRID_W + 2, GRID_H + 2);
}

function drawFrog(cx, cy, size, color) {
    var s = size;
    // Body
    ctx.fillStyle = color;
    ctx.fillRect(cx - s/2, cy - s/2, s, s);
    // Darker legs
    ctx.fillStyle = "#558b2f";
    ctx.fillRect(cx - s/2 - 2, cy - s/4, 6, s/2);
    ctx.fillRect(cx + s/2 - 4, cy - s/4, 6, s/2);
    // Eyes
    ctx.fillStyle = "#fff";
    ctx.fillRect(cx - s/3, cy - s/2 - 2, s/5, s/5);
    ctx.fillRect(cx + s/3 - s/5, cy - s/2 - 2, s/5, s/5);
    ctx.fillStyle = "#000";
    ctx.fillRect(cx - s/3 + 2, cy - s/2, 3, 3);
    ctx.fillRect(cx + s/3 - s/5 + 2, cy - s/2, 3, 3);
}

// =========================================================================
// Screens
// =========================================================================
var Screens = (function() {
    var current = "title";
    var menuIndex = 0;
    var overlay = null;
    var activeScreenId = "title";

    function showOverlay(id) {
        if (!overlay) overlay = document.getElementById("overlay");
        var divs = overlay.children;
        for (var i = 0; i < divs.length; i++) divs[i].style.display = "none";
        var el = document.getElementById("screen-" + id);
        if (el) el.style.display = "block";
        overlay.style.display = "block";
        activeScreenId = id;
    }
    function hideOverlay() {
        if (!overlay) overlay = document.getElementById("overlay");
        overlay.style.display = "none";
    }
    function showHUD() { var el = document.getElementById("hud"); if (el) el.style.display = "flex"; }
    function hideHUD() { var el = document.getElementById("hud"); if (el) el.style.display = "none"; }

    function getMenuItems(id) {
        var el = document.getElementById("screen-" + id);
        if (!el) return [];
        var items = [];
        var containers = el.querySelectorAll(".menu-items");
        for (var ci = 0; ci < containers.length; ci++) {
            var children = containers[ci].children;
            for (var i = 0; i < children.length; i++) {
                if (children[i].className.indexOf("menu-item") !== -1) items.push(children[i]);
            }
        }
        return items;
    }

    function updateSelection(id) {
        var items = getMenuItems(id);
        for (var i = 0; i < items.length; i++) {
            items[i].className = (i === menuIndex) ? "menu-item selected" : "menu-item";
        }
    }

    function menuNav(id, key, onSelect) {
        var items = getMenuItems(id);
        if (items.length === 0) return;
        if (key === "ArrowUp") {
            menuIndex = (menuIndex - 1 + items.length) % items.length;
            updateSelection(id);
            Audio.menu();
        } else if (key === "ArrowDown") {
            menuIndex = (menuIndex + 1) % items.length;
            updateSelection(id);
            Audio.menu();
        } else if (key === "Enter") {
            Audio.select();
            if (onSelect) onSelect(menuIndex, items[menuIndex]);
        }
    }

    function switchTo(name) {
        current = name;
        if (name === "title") {
            menuIndex = 0;
            showOverlay("title");
            updateSelection("title");
            hideHUD();
        } else if (name === "howto") {
            menuIndex = 0;
            showOverlay("howto");
            updateSelection("howto");
            hideHUD();
        } else if (name === "playing") {
            hideOverlay();
            showHUD();
            Game.reset();
            updateHUD();
        } else if (name === "gameover") {
            hideHUD();
            var statsEl = document.getElementById("gameover-stats");
            if (statsEl) {
                var lines = [];
                lines.push("Score: " + Game.score);
                lines.push("Level: " + Game.level);
                lines.push("Pads: " + Game.padsFilled + "/" + Game.pads.length);
                if (Game.score >= Store.highScore && Game.score > 0) {
                    lines.push("");
                    lines.push("\u2605 NEW HIGH SCORE \u2605");
                } else {
                    lines.push("Best: " + Store.highScore);
                }
                statsEl.textContent = lines.join("\n");
            }
            menuIndex = 0;
            showOverlay("gameover");
            updateSelection("gameover");
        }
    }

    // keydown accepts DOM key strings — the app maps lib/input actions to
    // ArrowUp/ArrowDown/ArrowLeft/ArrowRight/Enter/Escape before calling.
    function keydown(key) {
        if (current === "title") {
            menuNav("title", key, function(idx) {
                if (idx === 0) switchTo("playing");
                else if (idx === 1) switchTo("howto");
                else if (idx === 2) { try { window.close(); } catch(e) {} }
            });
        } else if (current === "howto") {
            if (key === "Escape") { switchTo("title"); return; }
            menuNav("howto", key, function() { switchTo("title"); });
        } else if (current === "gameover") {
            menuNav("gameover", key, function(idx) {
                if (idx === 0) switchTo("playing");
                else if (idx === 1) switchTo("title");
            });
        } else if (current === "playing") {
            if (key === "Escape") { switchTo("title"); return; }
            if (key === "ArrowUp") hop(0, -1);
            else if (key === "ArrowDown") hop(0, 1);
            else if (key === "ArrowLeft") hop(-1, 0);
            else if (key === "ArrowRight") hop(1, 0);
        }
    }

    function init() {
        overlay = document.getElementById("overlay");

        overlay.addEventListener("mousemove", function(e) {
            if (!activeScreenId) return;
            var target = e.target;
            while (target && target !== overlay) {
                if (target.className && target.className.indexOf("menu-item") !== -1) break;
                target = target.parentNode;
            }
            if (!target || target === overlay) return;
            var items = getMenuItems(activeScreenId);
            for (var i = 0; i < items.length; i++) {
                if (items[i] === target) {
                    if (menuIndex !== i) {
                        menuIndex = i;
                        updateSelection(activeScreenId);
                        Audio.menu();
                    }
                    break;
                }
            }
        });

        overlay.addEventListener("click", function(e) {
            if (!activeScreenId) return;
            var target = e.target;
            while (target && target !== overlay) {
                if (target.className && target.className.indexOf("menu-item") !== -1) break;
                target = target.parentNode;
            }
            if (!target || target === overlay) return;
            var items = getMenuItems(activeScreenId);
            for (var i = 0; i < items.length; i++) {
                if (items[i] === target) {
                    menuIndex = i;
                    updateSelection(activeScreenId);
                    keydown("Enter");
                    break;
                }
            }
        });
    }

    return {
        init: init,
        switchTo: switchTo,
        keydown: keydown,
        getName: function() { return current; }
    };
})();

// =========================================================================
// Main loop
// =========================================================================
Audio.init();
Store.load();

Input.init([
    { name: "up",      label: "Hop Up",    defaults: ["w", "ArrowUp"] },
    { name: "down",    label: "Hop Down",  defaults: ["s", "ArrowDown"] },
    { name: "left",    label: "Hop Left",  defaults: ["a", "ArrowLeft"] },
    { name: "right",   label: "Hop Right", defaults: ["d", "ArrowRight"] },
    { name: "confirm", label: "Confirm",   defaults: ["Enter", " "] },
    { name: "pause",   label: "Menu",      defaults: ["Escape"] },
]);
Input.attach(window);

Screens.init();

Input.onAction(function(action, phase) {
    if (phase !== "down" || !action) return;
    if (action === "up")         Screens.keydown("ArrowUp");
    else if (action === "down")  Screens.keydown("ArrowDown");
    else if (action === "left")  Screens.keydown("ArrowLeft");
    else if (action === "right") Screens.keydown("ArrowRight");
    else if (action === "confirm") Screens.keydown("Enter");
    else if (action === "pause")   Screens.keydown("Escape");
});

Screens.switchTo("title");
GameLoop.create({
    tick: function(dt) {
        var W = getW(), H = getH();
        recalcLayout(W, H);
        if (Screens.getName() === "playing") update(dt);
    },
    draw: function() {
        var W = getW(), H = getH();
        if (Screens.getName() === "playing" || Screens.getName() === "gameover") {
            draw(W, H);
        } else {
            ctx.fillStyle = "#06060a";
            ctx.fillRect(0, 0, W, H);
        }
    },
}).start();
console.log("Hopper loaded");
})();
