// tilemap.js — fixed-size tile grid with per-tile draw callback and AABB queries.
//
// Stored as a flat Uint16Array (row-major). Tile id 0 is reserved for empty
// space; every non-zero id is rendered by the caller-supplied drawTile
// callback. Solidity is a boolean lookup keyed by id.
//
// Why a callback instead of an atlas image: bro's CanvasRenderingContext2D
// drawImage only accepts loaded Image objects (not arbitrary <canvas>
// elements as image sources), so apps that want code-driven art draw tiles
// directly with fillRect / paths each frame. Pre-baked PNG atlases work
// too — just do `drawTile = (ctx, id, x, y, s) => ctx.drawImage(img, ...)`.
//
// Usage:
//   <script src="../lib/tilemap.js"></script>
//   const tm = Tilemap.create({
//       tileSize: 32, cols: 60, rows: 18,
//       drawTile: (ctx, id, x, y, s) => Art.drawTile(ctx, id, x, y, s),
//       solidIds: [1, 2, 3],
//   });
//   tm.setRows(["....", "####", ...], { '.': 0, '#': 1 });
//   tm.draw(ctx, camX, camY, viewW, viewH);
//   if (tm.solidAtPx(px, py)) { ... }

(function (global) {
    'use strict';

    function create(opts) {
        opts = opts || {};
        const tileSize = opts.tileSize || 32;
        const cols     = opts.cols     || 1;
        const rows     = opts.rows     || 1;
        const data     = opts.data     || new Uint16Array(cols * rows);
        const solid    = new Uint8Array(256);
        for (const id of (opts.solidIds || [])) solid[id] = 1;

        let drawTile = opts.drawTile || null;

        function idx(c, r) { return r * cols + c; }
        function get(c, r) {
            if (c < 0 || c >= cols || r < 0 || r >= rows) return 0;
            return data[idx(c, r)];
        }
        function set(c, r, id) {
            if (c < 0 || c >= cols || r < 0 || r >= rows) return;
            data[idx(c, r)] = id;
        }
        function solidAt(c, r) {
            // Out-of-bounds horizontally = solid (walls). Below the floor = empty
            // (game decides what falling-out means, not the tilemap).
            if (c < 0 || c >= cols) return true;
            if (r < 0 || r >= rows) return false;
            return !!solid[data[idx(c, r)]];
        }
        function solidAtPx(px, py) {
            return solidAt(Math.floor(px / tileSize), Math.floor(py / tileSize));
        }

        function setRows(rowStrings, charMap) {
            for (let r = 0; r < rowStrings.length && r < rows; r++) {
                const s = rowStrings[r];
                for (let c = 0; c < s.length && c < cols; c++) {
                    const id = charMap[s[c]];
                    if (id !== undefined) data[idx(c, r)] = id;
                }
            }
        }

        function draw(ctx, camX, camY, viewW, viewH) {
            if (!drawTile) return;
            const c0 = Math.max(0, Math.floor(camX / tileSize));
            const r0 = Math.max(0, Math.floor(camY / tileSize));
            const c1 = Math.min(cols - 1, Math.ceil((camX + viewW) / tileSize));
            const r1 = Math.min(rows - 1, Math.ceil((camY + viewH) / tileSize));
            for (let r = r0; r <= r1; r++) {
                for (let c = c0; c <= c1; c++) {
                    const id = data[idx(c, r)];
                    if (!id) continue;
                    drawTile(ctx, id,
                             Math.round(c * tileSize - camX),
                             Math.round(r * tileSize - camY),
                             tileSize);
                }
            }
        }

        return {
            tileSize, cols, rows, data,
            widthPx:  cols * tileSize,
            heightPx: rows * tileSize,
            get, set, setRows, solidAt, solidAtPx, draw,
            setSolid(id, isSolid) { solid[id] = isSolid ? 1 : 0; },
            setDrawTile(fn) { drawTile = fn; },
        };
    }

    global.Tilemap = { create };
})(typeof window !== 'undefined' ? window : globalThis);
