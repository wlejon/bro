// ghosts.js — ghost AI
// 4 ghosts: Blinky (red, chase), Pinky (pink, target ahead), Inky (cyan, random), Clyde (orange, mixed)
var P = P || {};

// Directions: right, left, up, down
P.DIRS = [
    { dx:  1, dy:  0, name: "right" },
    { dx: -1, dy:  0, name: "left" },
    { dx:  0, dy: -1, name: "up" },
    { dx:  0, dy:  1, name: "down" }
];

P.opposite = function(d) {
    if (d === 0) return 1;
    if (d === 1) return 0;
    if (d === 2) return 3;
    return 2;
};

P.Ghost = function(name, color, cornerC, cornerR, personality, spawnDelay) {
    this.name = name;
    this.color = color;
    this.corner = { c: cornerC, r: cornerR };
    this.personality = personality; // "chase","ahead","random","mixed"
    this.spawnDelay = spawnDelay;   // ms before leaving house
    this.reset();
};

P.Ghost.prototype.reset = function() {
    // Position in tile coords (float)
    this.c = P.Maze.ghostHouse.c;
    this.r = P.Maze.ghostHouse.r;
    this.dir = 2; // up
    this.mode = "house";        // "house","leaving","chase","scatter","frightened","eaten"
    this.houseTimer = this.spawnDelay;
    this.frightenedTimer = 0;
    this.eatenBonusIdx = 0;
    this.speed = 1.0; // tiles per second base (scaled in update)
};

P.Ghost.prototype.setFrightened = function(duration) {
    if (this.mode === "eaten" || this.mode === "house" || this.mode === "leaving") return;
    this.mode = "frightened";
    this.frightenedTimer = duration;
    // reverse direction
    this.dir = P.opposite(this.dir);
};

P.Ghost.prototype.getTargetTile = function(pac) {
    if (this.mode === "eaten") {
        return { c: P.Maze.ghostHouse.c, r: P.Maze.ghostHouse.r };
    }
    if (this.mode === "scatter") {
        return this.corner;
    }
    // chase-like
    switch (this.personality) {
        case "chase":
            return { c: pac.c, r: pac.r };
        case "ahead": {
            var dir = P.DIRS[pac.dir];
            return { c: pac.c + dir.dx * 4, r: pac.r + dir.dy * 4 };
        }
        case "mixed": {
            // if far from pacman chase; if close, go to corner
            var dc = pac.c - this.c, dr = pac.r - this.r;
            var dist2 = dc * dc + dr * dr;
            if (dist2 > 64) return { c: pac.c, r: pac.r };
            return this.corner;
        }
        case "random":
        default:
            return null; // random pick
    }
};

// Choose a next direction at a tile intersection.
// Ghosts can't reverse.
P.Ghost.prototype.chooseDir = function(pac) {
    var ci = Math.round(this.c);
    var ri = Math.round(this.r);
    var candidates = [];
    for (var i = 0; i < 4; i++) {
        if (i === P.opposite(this.dir)) continue;
        var d = P.DIRS[i];
        var nc = P.Maze.wrapCol(ci + d.dx);
        var nr = ri + d.dy;
        var allowDoor = (this.mode === "eaten" || this.mode === "leaving");
        if (P.Maze.isPassableForGhost(nc, nr, allowDoor)) {
            candidates.push({ i: i, c: nc, r: nr });
        }
    }
    if (candidates.length === 0) {
        // reverse as fallback
        return P.opposite(this.dir);
    }
    if (this.mode === "frightened") {
        return candidates[Math.floor(Math.random() * candidates.length)].i;
    }
    var target = this.getTargetTile(pac);
    if (!target) {
        return candidates[Math.floor(Math.random() * candidates.length)].i;
    }
    // pick candidate with min euclid dist to target
    var best = candidates[0];
    var bestD = Infinity;
    for (var j = 0; j < candidates.length; j++) {
        var cand = candidates[j];
        var dc = cand.c - target.c;
        var dr = cand.r - target.r;
        var d2 = dc * dc + dr * dr;
        if (d2 < bestD) { bestD = d2; best = cand; }
    }
    return best.i;
};

P.Ghost.prototype.update = function(dt, pac) {
    var dtS = dt / 1000;

    // Handle house/leaving/eaten transitions
    if (this.mode === "house") {
        this.houseTimer -= dt;
        // bob up and down inside house
        this.r += Math.sin(this.houseTimer * 0.005) * 0.01;
        if (this.houseTimer <= 0) {
            this.mode = "leaving";
            this.c = P.Maze.ghostHouse.c;
        }
        return;
    }

    if (this.mode === "leaving") {
        // move toward door, then one tile past it into the corridor, then start roaming
        var door = P.Maze.ghostHouseDoor;
        var exit = { c: door.c, r: door.r - 1 }; // one row above the door
        var dc = exit.c - this.c;
        var dr = exit.r - this.r;
        var dist = Math.sqrt(dc * dc + dr * dr);
        var spd = 3.0 * dtS;
        if (dist < spd) {
            this.c = exit.c;
            this.r = exit.r;
            this.mode = "chase";
            this.dir = (Math.random() < 0.5) ? 1 : 0; // left or right
            return;
        }
        this.c += (dc / dist) * spd;
        this.r += (dr / dist) * spd;
        return;
    }

    if (this.mode === "eaten") {
        // head to ghost house
        var target = P.Maze.ghostHouse;
        var dc = target.c - this.c;
        var dr = target.r - this.r;
        var dist = Math.sqrt(dc * dc + dr * dr);
        var spd = 8.0 * dtS;
        if (dist < 0.2) {
            this.c = target.c;
            this.r = target.r;
            this.mode = "leaving";
            this.houseTimer = 0;
            return;
        }
        // To keep it simple, use direct pathfinding - but only through passable tiles + doors
        // Use greedy nav — pick dir that reduces distance most and is passable
        var ci = Math.round(this.c);
        var ri = Math.round(this.r);
        var best = null, bestD = Infinity;
        for (var i = 0; i < 4; i++) {
            var d = P.DIRS[i];
            var nc = P.Maze.wrapCol(ci + d.dx);
            var nr = ri + d.dy;
            if (!P.Maze.isPassableForGhost(nc, nr, true)) continue;
            var ddc = nc - target.c;
            var ddr = nr - target.r;
            var d2 = ddc * ddc + ddr * ddr;
            if (d2 < bestD) { bestD = d2; best = i; }
        }
        if (best !== null) this.dir = best;
        var dd = P.DIRS[this.dir];
        this.c = P.Maze.wrapCol(this.c + dd.dx * spd);
        this.r += dd.dy * spd;
        return;
    }

    // frightened timer
    if (this.mode === "frightened") {
        this.frightenedTimer -= dt;
        if (this.frightenedTimer <= 0) this.mode = "chase";
    }

    // Speed
    var base = (this.mode === "frightened") ? 4.5 : 6.5;
    var step = base * dtS;

    // At a tile center: choose a new direction
    var ci = Math.round(this.c);
    var ri = Math.round(this.r);
    var centerEps = step * 0.5;
    var atCenter = Math.abs(this.c - ci) < centerEps && Math.abs(this.r - ri) < centerEps;
    if (atCenter) {
        this.c = ci; this.r = ri;
        this.dir = this.chooseDir(pac);
    }

    var d = P.DIRS[this.dir];
    // Prevent walking through walls mid-motion
    var nextC = P.Maze.wrapCol(ci + d.dx);
    var nextR = ri + d.dy;
    if (!P.Maze.isPassableForGhost(nextC, nextR, false)) {
        // snap and re-pick
        this.c = ci; this.r = ri;
        this.dir = this.chooseDir(pac);
        d = P.DIRS[this.dir];
    }
    this.c = this.c + d.dx * step;
    this.r = this.r + d.dy * step;
    this.c = P.Maze.wrapCol(this.c);
};

P.Ghost.prototype.draw = function(ctx, ox, oy, tile, globalFrightBlink) {
    var cx = ox + this.c * tile + tile / 2;
    var cy = oy + this.r * tile + tile / 2;
    var rad = tile * 0.45;

    if (this.mode === "eaten") {
        // just eyes
        this.drawEyes(ctx, cx, cy, rad);
        return;
    }

    var body = this.color;
    if (this.mode === "frightened") {
        body = globalFrightBlink ? "#ffffff" : "#2121ff";
    }
    ctx.fillStyle = body;
    ctx.beginPath();
    ctx.arc(cx, cy - rad * 0.1, rad, Math.PI, 0, false);
    // body rectangle
    ctx.lineTo(cx + rad, cy + rad * 0.6);
    // scalloped bottom
    var waves = 3;
    for (var i = 0; i < waves; i++) {
        var sx = cx + rad - (2 * rad / waves) * (i);
        var ex = cx + rad - (2 * rad / waves) * (i + 1);
        var mx = (sx + ex) / 2;
        ctx.quadraticCurveTo(mx, cy + rad * 0.3, ex, cy + rad * 0.6);
    }
    ctx.lineTo(cx - rad, cy - rad * 0.1);
    ctx.closePath();
    ctx.fill();

    if (this.mode === "frightened") {
        // scared face
        ctx.fillStyle = globalFrightBlink ? "#ff0000" : "#ffffff";
        // eyes
        ctx.beginPath();
        ctx.arc(cx - rad * 0.35, cy - rad * 0.15, rad * 0.15, 0, Math.PI * 2);
        ctx.arc(cx + rad * 0.35, cy - rad * 0.15, rad * 0.15, 0, Math.PI * 2);
        ctx.fill();
        // mouth zigzag
        ctx.strokeStyle = globalFrightBlink ? "#ff0000" : "#ffffff";
        ctx.lineWidth = Math.max(1, tile * 0.07);
        ctx.beginPath();
        var mw = rad * 0.7;
        ctx.moveTo(cx - mw / 2, cy + rad * 0.2);
        ctx.lineTo(cx - mw / 4, cy + rad * 0.05);
        ctx.lineTo(cx, cy + rad * 0.2);
        ctx.lineTo(cx + mw / 4, cy + rad * 0.05);
        ctx.lineTo(cx + mw / 2, cy + rad * 0.2);
        ctx.stroke();
    } else {
        this.drawEyes(ctx, cx, cy, rad);
    }
};

P.Ghost.prototype.drawEyes = function(ctx, cx, cy, rad) {
    var d = P.DIRS[this.dir];
    var ex = d.dx * rad * 0.15;
    var ey = d.dy * rad * 0.15;
    ctx.fillStyle = "#ffffff";
    ctx.beginPath();
    ctx.arc(cx - rad * 0.35, cy - rad * 0.15, rad * 0.22, 0, Math.PI * 2);
    ctx.arc(cx + rad * 0.35, cy - rad * 0.15, rad * 0.22, 0, Math.PI * 2);
    ctx.fill();
    ctx.fillStyle = "#2121ff";
    ctx.beginPath();
    ctx.arc(cx - rad * 0.35 + ex, cy - rad * 0.15 + ey, rad * 0.1, 0, Math.PI * 2);
    ctx.arc(cx + rad * 0.35 + ex, cy - rad * 0.15 + ey, rad * 0.1, 0, Math.PI * 2);
    ctx.fill();
};

P.Ghosts = {
    list: [],

    init: function() {
        this.list = [
            new P.Ghost("scarlet", "#ff0000", P.Maze.COLS - 2, 1, "chase", 0),
            new P.Ghost("rose",    "#ffb8ff", 1,               1, "ahead", 2000),
            new P.Ghost("azure",   "#00ffff", P.Maze.COLS - 2, P.Maze.ROWS - 2, "random", 5000),
            new P.Ghost("amber",   "#ffb852", 1,               P.Maze.ROWS - 2, "mixed", 8000)
        ];
    },

    resetAll: function() {
        for (var i = 0; i < this.list.length; i++) this.list[i].reset();
    },

    frightenAll: function(duration) {
        for (var i = 0; i < this.list.length; i++) this.list[i].setFrightened(duration);
    },

    update: function(dt, pac) {
        for (var i = 0; i < this.list.length; i++) this.list[i].update(dt, pac);
    },

    draw: function(ctx, ox, oy, tile, blink) {
        for (var i = 0; i < this.list.length; i++) this.list[i].draw(ctx, ox, oy, tile, blink);
    }
};
