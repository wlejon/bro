// maze.js — grid, walls, pellets
// Tiles:
//   '#' = wall
//   '.' = pellet
//   'o' = power pellet
//   ' ' = empty corridor (no pellet — ghost house area, tunnels)
//   '-' = ghost house door
//   'T' = tunnel opening (empty, wraps)
var P = P || {};

P.Maze = {
    COLS: 28,
    ROWS: 31,
    // 28 columns wide. Classic-inspired layout.
    LAYOUT: [
        "############################",
        "#............##............#",
        "#.####.#####.##.#####.####.#",
        "#o####.#####.##.#####.####o#",
        "#.####.#####.##.#####.####.#",
        "#..........................#",
        "#.####.##.########.##.####.#",
        "#.####.##.########.##.####.#",
        "#......##....##....##......#",
        "######.#####.##.#####.######",
        "######.#####.##.#####.######",
        "######.##..........##.######",
        "######.##.###--###.##.######",
        "######.##.#      #.##.######",
        "T     .   #      #   .     T",
        "######.##.#      #.##.######",
        "######.##.########.##.######",
        "######.##..........##.######",
        "######.##.########.##.######",
        "######.##.########.##.######",
        "#............##............#",
        "#.####.#####.##.#####.####.#",
        "#.####.#####.##.#####.####.#",
        "#o..##................##..o#",
        "###.##.##.########.##.##.###",
        "###.##.##.########.##.##.###",
        "#......##....##....##......#",
        "#.##########.##.##########.#",
        "#.##########.##.##########.#",
        "#..........................#",
        "############################"
    ],

    grid: null,       // 2d array of tile chars, with pellets removed as eaten
    pelletCount: 0,
    totalPellets: 0,

    // pacman spawn (column, row)
    pacmanSpawn: { c: 13.5, r: 23 },
    // ghost house center (used for spawn/respawn)
    ghostHouse: { c: 13.5, r: 14 },
    ghostHouseDoor: { c: 13.5, r: 12 },

    tunnelRow: 14,

    reset: function() {
        this.grid = [];
        this.pelletCount = 0;
        for (var r = 0; r < this.ROWS; r++) {
            var row = [];
            var line = this.LAYOUT[r] || "";
            for (var c = 0; c < this.COLS; c++) {
                var ch = c < line.length ? line.charAt(c) : ' ';
                if (ch === '.' || ch === 'o') this.pelletCount++;
                row.push(ch);
            }
            this.grid.push(row);
        }
        this.totalPellets = this.pelletCount;
    },

    // normalize column (handles tunnel wrap)
    wrapCol: function(c) {
        if (c < 0) return this.COLS + c;
        if (c >= this.COLS) return c - this.COLS;
        return c;
    },

    tileAt: function(c, r) {
        if (r < 0 || r >= this.ROWS) return '#';
        c = this.wrapCol(c);
        var row = this.grid[r];
        if (!row) return '#';
        return row[c];
    },

    // can Pac-Man walk onto this tile?
    isPassableForPac: function(c, r) {
        var t = this.tileAt(c, r);
        return t !== '#' && t !== '-';
    },

    // ghosts: door is passable (but normally only used when entering/leaving house)
    isPassableForGhost: function(c, r, allowDoor) {
        var t = this.tileAt(c, r);
        if (t === '#') return false;
        if (t === '-') return !!allowDoor;
        return true;
    },

    eatPelletAt: function(c, r) {
        c = this.wrapCol(c);
        if (r < 0 || r >= this.ROWS) return null;
        var row = this.grid[r];
        var t = row[c];
        if (t === '.' || t === 'o') {
            row[c] = ' ';
            this.pelletCount--;
            return t;
        }
        return null;
    },

    // Draw the maze walls and pellets
    // tileSize: pixels
    // ox, oy: origin in pixels
    draw: function(ctx, ox, oy, tileSize) {
        // walls: precompute a thin-line wall style
        ctx.strokeStyle = "#2121ff";
        ctx.lineWidth = Math.max(2, tileSize * 0.14);
        ctx.lineCap = "square";

        for (var r = 0; r < this.ROWS; r++) {
            for (var c = 0; c < this.COLS; c++) {
                var t = this.grid[r][c];
                var x = ox + c * tileSize;
                var y = oy + r * tileSize;
                if (t === '#') {
                    // Draw as filled block with slight rounding for nice look
                    ctx.fillStyle = "#1a1aff";
                    ctx.fillRect(x, y, tileSize, tileSize);
                    // Inner dark to give wall-line appearance
                    ctx.fillStyle = "#000";
                    var m = Math.max(1, tileSize * 0.18);
                    // Determine neighbors for inset
                    var top = this.tileAt(c, r - 1) === '#';
                    var bot = this.tileAt(c, r + 1) === '#';
                    var lef = this.tileAt(c - 1, r) === '#';
                    var rig = this.tileAt(c + 1, r) === '#';
                    // Only draw inner core if isolated enough — simpler: just draw top bar of bright
                    // Keep it as flat solid blue to be reliable
                    ctx.fillStyle = "#1a1aff";
                    // (revert — keep solid)
                } else if (t === '-') {
                    ctx.fillStyle = "#ff69b4";
                    ctx.fillRect(x, y + tileSize * 0.45, tileSize, tileSize * 0.1);
                }
            }
        }

        // Pellets on top
        for (var r = 0; r < this.ROWS; r++) {
            for (var c = 0; c < this.COLS; c++) {
                var t = this.grid[r][c];
                var cx = ox + c * tileSize + tileSize / 2;
                var cy = oy + r * tileSize + tileSize / 2;
                if (t === '.') {
                    ctx.fillStyle = "#ffd7a8";
                    ctx.beginPath();
                    ctx.arc(cx, cy, Math.max(1.2, tileSize * 0.1), 0, Math.PI * 2);
                    ctx.fill();
                } else if (t === 'o') {
                    ctx.fillStyle = "#ffd7a8";
                    ctx.beginPath();
                    ctx.arc(cx, cy, Math.max(3, tileSize * 0.3), 0, Math.PI * 2);
                    ctx.fill();
                }
            }
        }
    }
};
