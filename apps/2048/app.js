// 2048 — main game, DOM rendering, input
(function() {
"use strict";

var SIZE = 4;

// Board geometry (matches style.css):
//   cell slot size 107.5px, gap 10px => stride 117.5px
var TILE_STRIDE = 117.5;

// --- Game state ---
// grid: 4x4 of tiles. Each tile is null or { value, id }
var nextId = 1;
function newTile(value) {
    return { value: value, id: nextId++ };
}

var state = {
    grid: null,
    score: 0,
    best: 0,
    won: false,
    keepPlaying: false,
    prev: null
};

var screenName = "title";

// Map of tile id -> DOM element (so we can reuse elements across moves for transitions)
var tileEls = {};

// --- Persistence ---
var store = Storage.create("2048");
function loadBest() {
    store.load({ best: 0 });
    state.best = store.get("best") || 0;
}
function saveBest() {
    store.set("best", state.best);
    store.save();
}

// --- Input ---
Input.init([
    { name: "up",      label: "Up",      defaults: ["w", "ArrowUp"] },
    { name: "down",    label: "Down",    defaults: ["s", "ArrowDown"] },
    { name: "left",    label: "Left",    defaults: ["a", "ArrowLeft"] },
    { name: "right",   label: "Right",   defaults: ["d", "ArrowRight"] },
    { name: "undo",    label: "Undo",    defaults: ["u"] },
    { name: "restart", label: "Restart", defaults: ["r"] },
    { name: "pause",   label: "Menu",    defaults: ["Escape"] },
    { name: "confirm", label: "Confirm", defaults: ["Enter", " "] },
]);
Input.attach(window);

// --- Grid helpers ---
function emptyGrid() {
    var g = new Array(SIZE);
    for (var r = 0; r < SIZE; r++) {
        g[r] = new Array(SIZE);
        for (var c = 0; c < SIZE; c++) g[r][c] = null;
    }
    return g;
}

function cloneGrid(g) {
    var ng = new Array(SIZE);
    for (var r = 0; r < SIZE; r++) {
        ng[r] = new Array(SIZE);
        for (var c = 0; c < SIZE; c++) {
            ng[r][c] = g[r][c] ? { value: g[r][c].value, id: g[r][c].id } : null;
        }
    }
    return ng;
}

function emptyCells(g) {
    var out = [];
    for (var r = 0; r < SIZE; r++) {
        for (var c = 0; c < SIZE; c++) {
            if (!g[r][c]) out.push({ r: r, c: c });
        }
    }
    return out;
}

function spawnTile(g) {
    var empties = emptyCells(g);
    if (empties.length === 0) return null;
    var spot = empties[Math.floor(Math.random() * empties.length)];
    var value = Math.random() < 0.1 ? 4 : 2;
    var tile = newTile(value);
    g[spot.r][spot.c] = tile;
    return { tile: tile, r: spot.r, c: spot.c };
}

function hasMoves(g) {
    for (var r = 0; r < SIZE; r++) {
        for (var c = 0; c < SIZE; c++) {
            if (!g[r][c]) return true;
            var v = g[r][c].value;
            if (c + 1 < SIZE && g[r][c+1] && g[r][c+1].value === v) return true;
            if (r + 1 < SIZE && g[r+1][c] && g[r+1][c].value === v) return true;
        }
    }
    return false;
}

function hasValue(g, target) {
    for (var r = 0; r < SIZE; r++) {
        for (var c = 0; c < SIZE; c++) {
            if (g[r][c] && g[r][c].value === target) return true;
        }
    }
    return false;
}

// --- Slide logic ---
// Returns { changed, mergedIds, gainedScore }
// mergedIds is a set of tile ids that just resulted from a merge (for pop animation).
function move(g, dir) {
    var gained = 0;
    var anyChange = false;
    var mergedIds = {};

    function processLine(cells) {
        var tiles = [];
        for (var i = 0; i < cells.length; i++) {
            var rc = cells[i];
            if (g[rc.r][rc.c]) tiles.push({ tile: g[rc.r][rc.c], r: rc.r, c: rc.c });
        }
        var resultTiles = [];
        var i2 = 0;
        var outIdx = 0;
        while (i2 < tiles.length) {
            var dest = cells[outIdx];
            var cur = tiles[i2];
            if (i2 + 1 < tiles.length && tiles[i2+1].tile.value === cur.tile.value) {
                var newVal = cur.tile.value * 2;
                gained += newVal;
                var merged = newTile(newVal);
                mergedIds[merged.id] = true;
                resultTiles.push(merged);
                anyChange = true;
                i2 += 2;
            } else {
                resultTiles.push(cur.tile);
                if (cur.r !== dest.r || cur.c !== dest.c) {
                    anyChange = true;
                }
                i2 += 1;
            }
            outIdx += 1;
        }
        for (var k = 0; k < cells.length; k++) {
            g[cells[k].r][cells[k].c] = k < resultTiles.length ? resultTiles[k] : null;
        }
    }

    for (var i = 0; i < SIZE; i++) {
        var line = [];
        if (dir === "left") {
            for (var c = 0; c < SIZE; c++) line.push({ r: i, c: c });
        } else if (dir === "right") {
            for (var c2 = SIZE - 1; c2 >= 0; c2--) line.push({ r: i, c: c2 });
        } else if (dir === "up") {
            for (var r = 0; r < SIZE; r++) line.push({ r: r, c: i });
        } else if (dir === "down") {
            for (var r2 = SIZE - 1; r2 >= 0; r2--) line.push({ r: r2, c: i });
        }
        processLine(line);
    }

    return { changed: anyChange, mergedIds: mergedIds, gainedScore: gained };
}

// --- DOM rendering ---
function tileClass(value) {
    if (value <= 2048) return "tile-" + value;
    return "tile-super";
}

function clearTiles() {
    var tilesEl = document.getElementById("tiles");
    if (tilesEl) tilesEl.innerHTML = "";
    tileEls = {};
}

function renderTiles(opts) {
    // opts: { mergedIds: {...}, newIds: {...} }
    var tilesEl = document.getElementById("tiles");
    if (!tilesEl) return;

    var mergedIds = (opts && opts.mergedIds) || {};
    var newIds = (opts && opts.newIds) || {};

    // Collect present ids in the grid
    var present = {};
    for (var r = 0; r < SIZE; r++) {
        for (var c = 0; c < SIZE; c++) {
            var t = state.grid[r][c];
            if (t) present[t.id] = { tile: t, r: r, c: c };
        }
    }

    // Remove DOM elements whose tile is no longer in grid
    var toRemove = [];
    for (var id in tileEls) {
        if (!present[id]) toRemove.push(id);
    }
    for (var i = 0; i < toRemove.length; i++) {
        var el = tileEls[toRemove[i]];
        if (el && el.parentNode) el.parentNode.removeChild(el);
        delete tileEls[toRemove[i]];
    }

    // Create/update DOM elements for each present tile
    for (var pid in present) {
        var info = present[pid];
        var tile = info.tile;
        var x = info.c * TILE_STRIDE;
        var y = info.r * TILE_STRIDE;
        var el = tileEls[tile.id];

        if (!el) {
            el = document.createElement("div");
            el.setAttribute("data-id", String(tile.id));
            var inner = document.createElement("div");
            inner.className = "tile-inner";
            inner.textContent = String(tile.value);
            el.appendChild(inner);
            tilesEl.appendChild(el);
            tileEls[tile.id] = el;

            // Set initial class + position (no transition on first paint for this tile)
            var cls = "tile " + tileClass(tile.value);
            if (newIds[tile.id]) cls += " tile-new";
            else if (mergedIds[tile.id]) cls += " tile-merged";
            el.className = cls;
            el.style.transform = "translate(" + x + "px, " + y + "px)";
        } else {
            // Update inner text and class in case value changed (shouldn't, but safe)
            var inner2 = el.firstChild;
            if (inner2) inner2.textContent = String(tile.value);
            var cls2 = "tile " + tileClass(tile.value);
            if (mergedIds[tile.id]) cls2 += " tile-merged";
            el.className = cls2;
            el.style.transform = "translate(" + x + "px, " + y + "px)";
        }
    }
}

// --- Game actions ---
function newGame() {
    state.grid = emptyGrid();
    state.score = 0;
    state.won = false;
    state.keepPlaying = false;
    state.prev = null;
    clearTiles();
    var s1 = spawnTile(state.grid);
    var s2 = spawnTile(state.grid);
    var newIds = {};
    if (s1) newIds[s1.tile.id] = true;
    if (s2) newIds[s2.tile.id] = true;
    updateHud();
    renderTiles({ newIds: newIds });
}

function undo() {
    if (!state.prev) return;
    state.grid = state.prev.grid;
    state.score = state.prev.score;
    state.won = state.prev.won;
    state.keepPlaying = state.prev.keepPlaying;
    state.prev = null;
    // After undo, tile ids may have changed entirely; wipe and rebuild.
    clearTiles();
    updateHud();
    renderTiles({});
}

function tryMove(dir) {
    if (screenName !== "playing") return;

    var snapGrid = cloneGrid(state.grid);
    var snapScore = state.score;
    var snapWon = state.won;
    var snapKeep = state.keepPlaying;

    var result = move(state.grid, dir);
    if (!result.changed) return;

    state.score += result.gainedScore;
    if (state.score > state.best) {
        state.best = state.score;
        saveBest();
    }

    state.prev = { grid: snapGrid, score: snapScore, won: snapWon, keepPlaying: snapKeep };

    // Spawn new tile
    var spawn = spawnTile(state.grid);
    var newIds = {};
    if (spawn) newIds[spawn.tile.id] = true;

    updateHud();
    renderTiles({ mergedIds: result.mergedIds, newIds: newIds });

    // Check win / game over (immediate — DOM transitions run concurrently)
    if (!state.won && !state.keepPlaying && hasValue(state.grid, 2048)) {
        state.won = true;
        showScreen("win");
        var ws = document.getElementById("win-stats");
        if (ws) ws.textContent = "Score: " + state.score + "   Best: " + state.best;
        return;
    }
    if (!hasMoves(state.grid)) {
        showScreen("gameover");
        var gs = document.getElementById("gameover-stats");
        if (gs) gs.textContent = "Score: " + state.score + "   Best: " + state.best;
    }
}

function updateHud() {
    var sEl = document.getElementById("hud-score");
    if (sEl) sEl.textContent = String(state.score);
    var bEl = document.getElementById("hud-best");
    if (bEl) bEl.textContent = String(state.best);
}

// --- Screens ---
function showScreen(name) {
    screenName = name;
    var screens = ["title", "howtoplay", "gameover", "win"];
    for (var i = 0; i < screens.length; i++) {
        var el = document.getElementById("screen-" + screens[i]);
        if (el) el.style.display = "none";
    }
    var overlay = document.getElementById("overlay");
    var hud = document.getElementById("hud");
    var gameRoot = document.getElementById("game-root");

    if (name === "playing") {
        if (overlay) overlay.style.display = "none";
        if (hud) hud.style.display = "block";
        if (gameRoot) gameRoot.style.display = "block";
        // Ensure board reflects current state
        renderTiles({});
    } else {
        if (overlay) overlay.style.display = "block";
        if (hud) hud.style.display = "none";
        if (gameRoot) gameRoot.style.display = "none";
        var target = document.getElementById("screen-" + name);
        if (target) target.style.display = "block";
        resetMenuSelection(target);
    }
}

function resetMenuSelection(screenEl) {
    if (!screenEl) return;
    var items = screenEl.querySelectorAll(".menu-item");
    for (var i = 0; i < items.length; i++) {
        if (i === 0) items[i].classList.add("selected");
        else items[i].classList.remove("selected");
    }
}

function getCurrentScreenEl() {
    if (screenName === "playing") return null;
    return document.getElementById("screen-" + screenName);
}

function getMenuItems(screenEl) {
    if (!screenEl) return [];
    var nl = screenEl.querySelectorAll(".menu-item");
    var arr = [];
    for (var i = 0; i < nl.length; i++) arr.push(nl[i]);
    return arr;
}

function getSelectedIndex(items) {
    for (var i = 0; i < items.length; i++) {
        if (items[i].classList.contains("selected")) return i;
    }
    return 0;
}

function setSelectedIndex(items, idx) {
    for (var i = 0; i < items.length; i++) {
        if (i === idx) items[i].classList.add("selected");
        else items[i].classList.remove("selected");
    }
}

function menuNav(delta) {
    var sc = getCurrentScreenEl();
    if (!sc) return;
    var items = getMenuItems(sc);
    if (items.length === 0) return;
    var idx = getSelectedIndex(items);
    idx = (idx + delta + items.length) % items.length;
    setSelectedIndex(items, idx);
}

function menuActivate() {
    var sc = getCurrentScreenEl();
    if (!sc) return;
    var items = getMenuItems(sc);
    if (items.length === 0) return;
    var idx = getSelectedIndex(items);
    var el = items[idx];
    doMenuAction(el.getAttribute("data-action"));
}

function doMenuAction(action) {
    if (!action) return;
    if (action === "play") {
        newGame();
        showScreen("playing");
    } else if (action === "howtoplay") {
        showScreen("howtoplay");
    } else if (action === "back") {
        showScreen("title");
    } else if (action === "restart") {
        newGame();
        showScreen("playing");
    } else if (action === "keepplaying") {
        state.keepPlaying = true;
        showScreen("playing");
    } else if (action === "quit") {
        showScreen("title");
    }
}

// --- Input ---
Input.onAction(function(action, phase) {
    if (phase !== "down" || !action) return;
    if (screenName === "playing") {
        if (action === "left")       tryMove("left");
        else if (action === "right") tryMove("right");
        else if (action === "up")    tryMove("up");
        else if (action === "down")  tryMove("down");
        else if (action === "undo")    undo();
        else if (action === "restart") newGame();
        else if (action === "pause")   showScreen("title");
    } else {
        if (action === "up")   menuNav(-1);
        else if (action === "down") menuNav(1);
        else if (action === "confirm") menuActivate();
        else if (action === "pause") {
            if (screenName === "howtoplay" || screenName === "gameover" || screenName === "win") {
                showScreen("title");
            }
        }
    }
});

function onClick(e) {
    var target = e.target;
    if (!target) return;
    if (target.classList && target.classList.contains("menu-item")) {
        var action = target.getAttribute("data-action");
        if (action) {
            var sc = getCurrentScreenEl();
            if (sc) {
                var items = getMenuItems(sc);
                for (var i = 0; i < items.length; i++) {
                    if (items[i] === target) setSelectedIndex(items, i);
                }
            }
            doMenuAction(action);
        }
    }
}

document.body.addEventListener("click", onClick);

// --- Start ---
loadBest();
updateHud();
// Initialize an empty grid so renderTiles() is safe before newGame().
state.grid = emptyGrid();
showScreen("title");

console.log("2048 loaded!");
})();
