// tilemap.js — fixed-size tile grid with per-tile draw callback, AABB queries,
// and optional pixel-level destructible terrain.
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
// Destructible mode (opts.destructible: true) adds:
//   - A per-pixel solidity bitmask covering the entire world (Uint32Array,
//     1 bit per pixel, ~270 KB for a 3840×576 world).
//   - solidAtPixel(px, py) for pixel-precise collision.
//   - damageBeam(x0, y0, x1, y1, thickness) and damageCircle(cx, cy, r)
//     to clear bits along a swept line or inside a disc; both report the
//     count of pixels and tiles cleared so callers can attribute reward.
//   - damageDiff() / applyDamageDiff(diff) for snapshot/restore — the diff
//     is a sparse Int32Array of (wordIdx, value) pairs against the pristine
//     baseline, so untouched terrain costs zero to snapshot.
//   - Rendering: visible tiles are stamped per-frame as in non-destructible
//     mode. Tiles with cleared pixels route through a tileSize-sized scratch
//     canvas: drawTile into scratch, then 'destination-out' fillRect runs
//     for each cleared row, then drawImage(scratch, ...) onto the main ctx.
//     The scratch is reused across tiles. Workers skip rendering; they only
//     maintain the bitmask.
//
// Usage:
//   <script src="../lib/tilemap.js"></script>
//   const tm = Tilemap.create({
//       tileSize: 32, cols: 60, rows: 18,
//       drawTile: (ctx, id, x, y, s) => Art.drawTile(ctx, id, x, y, s),
//       solidIds: [1, 2, 3],
//       destructible: true,
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
        // Indestructible tile ids resist damageBeam/damageCircle: pixels
        // belonging to these tiles never clear, even though the bits are
        // present in the bitmask. Beams still *stop* on them (so explosions
        // place correctly), they just don't carve.
        const indestructible = new Uint8Array(256);
        for (const id of (opts.indestructibleIds || [])) indestructible[id] = 1;
        const destructible = !!opts.destructible;
        // Renderer-side bookkeeping. The damagedTiles Set drives the scratch-
        // canvas pass in draw(); workers never render, so they can opt out
        // and skip the per-bit Set.add and the per-restore rebuild scan.
        // This is a major MCTS perf lever once the agent starts firing —
        // saveDamageSnapshot/restoreDamageSnapshot run 24× per decision in
        // the live worker and rebuilding tile sets each time is wasted work.
        const trackDamagedTiles = opts.trackDamagedTiles !== false;
        const widthPx  = cols * tileSize;
        const heightPx = rows * tileSize;
        const wordsPerRow = (widthPx + 31) >> 5;

        let drawTile = opts.drawTile || null;

        // Destruction state — only allocated when destructible.
        let bitmask      = null;   // Uint32Array(wordsPerRow * heightPx)
        let pristineMask = null;   // Frozen baseline; diff is bitmask vs this
        let damagedWords = null;   // Map<wordIdx, currentValue> for words diverged from pristine
        let damagedTiles = null;   // Set<tileIdx> — tiles that need scratch-canvas rendering
        let scratchCanvas = null;  // Main thread only — reused per damaged tile
        let scratchCtx   = null;
        let pixelsCleared = 0;

        // Overlay shapes — "negative regions" treated as cleared on top of
        // the bitmask. fireOneBeam pushes here instead of carving pixels;
        // saveOverlaySnapshot/restoreOverlaySnapshot are O(1) (record/truncate
        // the count) so MCTS save/restore doesn't iterate any pixels. After a
        // real (non-rollout) action the caller commitOverlays(), which bakes
        // the shapes into the bitmask in a single pass — pixel iteration cost
        // is paid once per real fire instead of once per MCTS iteration.
        // Each entry has shape-specific fields plus precomputed query helpers.
        const overlays = [];        // pool reused across episodes
        let overlayCount = 0;       // logical length (entries [0..overlayCount-1])
        let overlaySaveCount = 0;   // saved length for MCTS save/restore

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
            if (bitmask) return solidAtPixel(px, py);
            return solidAt(Math.floor(px / tileSize), Math.floor(py / tileSize));
        }
        function solidAtPixel(px, py) {
            // Pixel-precise solidity. Out-of-bounds horizontally = solid wall;
            // out-of-bounds vertically below = empty (let the caller decide
            // what falling out of the world means).
            if (px < 0 || px >= widthPx) return true;
            if (py < 0 || py >= heightPx) return false;
            if (!bitmask) {
                return solidAt(Math.floor(px / tileSize), Math.floor(py / tileSize));
            }
            const ipx = px | 0, ipy = py | 0;
            const w = bitmask[ipy * wordsPerRow + (ipx >> 5)];
            if (((w >>> (ipx & 31)) & 1) === 0) return false;
            // Overlay scan: if any pending negative shape covers this pixel,
            // treat it as cleared. Count is small (MCTS rolls back via
            // truncateOverlays; commit bakes them into the bitmask).
            for (let i = 0; i < overlayCount; i++) {
                const ov = overlays[i];
                if (ov.kind === 0) {
                    // circle
                    const dx = ipx - ov.cx;
                    const dy = ipy - ov.cy;
                    if (dx * dx + dy * dy <= ov.r2) return false;
                } else {
                    // beam: rotated rectangle (length × 2*halfThickness)
                    const rx = ipx - ov.x0;
                    const ry = ipy - ov.y0;
                    const along = rx * ov.ux + ry * ov.uy;
                    if (along < 0 || along > ov.len) continue;
                    const perp = rx * ov.nx + ry * ov.ny;
                    if (perp < -ov.half || perp > ov.half) continue;
                    return false;
                }
            }
            return true;
        }

        function setRows(rowStrings, charMap) {
            for (let r = 0; r < rowStrings.length && r < rows; r++) {
                const s = rowStrings[r];
                for (let c = 0; c < s.length && c < cols; c++) {
                    const id = charMap[s[c]];
                    if (id !== undefined) data[idx(c, r)] = id;
                }
            }
            if (destructible) buildBitmask();
        }

        // ── Destruction ────────────────────────────────────────────────────
        function buildBitmask() {
            bitmask = new Uint32Array(wordsPerRow * heightPx);
            // For each solid tile, set every bit covering its tile rectangle.
            // Tile-aligned writes can mostly be done as full-uint32 stamps
            // when tileSize is a multiple of 32 (the common case: 32/64/128).
            for (let r = 0; r < rows; r++) {
                for (let c = 0; c < cols; c++) {
                    if (!solid[data[idx(c, r)]]) continue;
                    const x0 = c * tileSize;
                    const y0 = r * tileSize;
                    for (let py = 0; py < tileSize; py++) {
                        const yy = y0 + py;
                        if (yy < 0 || yy >= heightPx) continue;
                        for (let px = 0; px < tileSize; px++) {
                            const xx = x0 + px;
                            if (xx < 0 || xx >= widthPx) continue;
                            const wIdx = yy * wordsPerRow + (xx >> 5);
                            bitmask[wIdx] |= (1 << (xx & 31));
                        }
                    }
                }
            }
            pristineMask = new Uint32Array(bitmask);
            damagedWords = new Map();
            damagedTiles = trackDamagedTiles ? new Set() : null;
            pixelsCleared = 0;
        }

        function ensureScratch() {
            if (scratchCtx || typeof document === 'undefined') return;
            scratchCanvas = document.createElement('canvas');
            scratchCanvas.width = tileSize;
            scratchCanvas.height = tileSize;
            scratchCtx = scratchCanvas.getContext('2d');
            scratchCtx.imageSmoothingEnabled = false;
        }

        // Recompute damagedTiles from the current bitmask vs pristine. Used
        // after applyDamageDiff/resetDamage, where a wholesale change makes
        // incremental tracking infeasible. Cost scales with damaged words.
        function rebuildDamagedTiles() {
            if (!damagedTiles) return;
            damagedTiles.clear();
            if (!damagedWords) return;
            for (const [wIdx, cur] of damagedWords) {
                const cleared = pristineMask[wIdx] & ~cur;
                if (!cleared) continue;
                const wordRow = (wIdx / wordsPerRow) | 0;
                const wordCol = wIdx - wordRow * wordsPerRow;
                const x0 = wordCol << 5;
                const r  = (wordRow / tileSize) | 0;
                // Bits within the word can span up to two adjacent tile cols
                // (when tileSize is not a multiple of 32). Mark every tile
                // touched by a cleared bit.
                for (let i = 0; i < 32; i++) {
                    if ((cleared >>> i) & 1) {
                        const c = ((x0 + i) / tileSize) | 0;
                        if (c >= 0 && c < cols && r >= 0 && r < rows) {
                            damagedTiles.add(r * cols + c);
                        }
                    }
                }
            }
        }

        // Clear one pixel in the bitmask. Returns true if a previously-set
        // bit was actually flipped (used to count pixelsCleared).
        function clearPixelBit(px, py) {
            if (px < 0 || px >= widthPx) return false;
            if (py < 0 || py >= heightPx) return false;
            const tc = (px / tileSize) | 0;
            const tr = (py / tileSize) | 0;
            if (indestructible[data[tr * cols + tc]]) return false;
            const wIdx = py * wordsPerRow + (px >> 5);
            const bit = 1 << (px & 31);
            const cur = bitmask[wIdx];
            if (!(cur & bit)) return false;
            const next = cur & ~bit;
            bitmask[wIdx] = next;
            if (next === pristineMask[wIdx]) damagedWords.delete(wIdx);
            else damagedWords.set(wIdx, next >>> 0);
            if (damagedTiles) damagedTiles.add(tr * cols + tc);
            return true;
        }

        // Carve a circle out of the bitmask. Returns the pixel count cleared.
        function damageCircle(cx, cy, r) {
            if (!destructible) return 0;
            const x0 = Math.max(0, Math.floor(cx - r));
            const y0 = Math.max(0, Math.floor(cy - r));
            const x1 = Math.min(widthPx - 1, Math.ceil(cx + r));
            const y1 = Math.min(heightPx - 1, Math.ceil(cy + r));
            const r2 = r * r;
            let cleared = 0;
            for (let py = y0; py <= y1; py++) {
                const dy = py - cy;
                const dy2 = dy * dy;
                for (let px = x0; px <= x1; px++) {
                    const dx = px - cx;
                    if (dx * dx + dy2 > r2) continue;
                    if (clearPixelBit(px, py)) cleared++;
                }
            }
            pixelsCleared += cleared;
            return cleared;
        }

        // Carve a thickness-wide swept-line out of the bitmask. Stops at the
        // first solid pixel along the centerline if `stopOnHit` is true and
        // returns where it stopped (so callers can place an explosion at the
        // impact point). When stopOnHit is false, the full line is carved.
        function damageBeam(x0, y0, x1, y1, thickness, stopOnHit) {
            const out = { cleared: 0, hitX: x1, hitY: y1, hit: false };
            if (!destructible) return out;
            const dx = x1 - x0, dy = y1 - y0;
            const len = Math.hypot(dx, dy);
            if (len < 1) return out;
            const ux = dx / len, uy = dy / len;
            const nx = -uy, ny = ux;
            const half = Math.max(1, thickness * 0.5);
            let stopped = -1;

            // Step along centerline at 1-pixel granularity. Look-ahead for
            // stopOnHit: a step is the first impact when any bit on the
            // centerline (ignoring perpendicular extent) is solid.
            if (stopOnHit) {
                for (let s = 0; s <= len; s++) {
                    const cx = x0 + ux * s;
                    const cy = y0 + uy * s;
                    const ipx = Math.round(cx);
                    const ipy = Math.round(cy);
                    if (ipx < 0 || ipx >= widthPx || ipy < 0 || ipy >= heightPx) {
                        stopped = s; break;
                    }
                    const w = bitmask[ipy * wordsPerRow + (ipx >> 5)];
                    if ((w >>> (ipx & 31)) & 1) { stopped = s; break; }
                }
                if (stopped < 0) stopped = len;
            } else {
                stopped = len;
            }

            // Now carve the band from s=0 to s=stopped.
            for (let s = 0; s <= stopped; s++) {
                const cx = x0 + ux * s;
                const cy = y0 + uy * s;
                for (let t = -half; t <= half; t += 1) {
                    const px = Math.round(cx + nx * t);
                    const py = Math.round(cy + ny * t);
                    if (clearPixelBit(px, py)) out.cleared++;
                }
            }
            pixelsCleared += out.cleared;

            const endX = x0 + ux * stopped;
            const endY = y0 + uy * stopped;
            out.hitX = endX;
            out.hitY = endY;
            out.hit  = stopped < len;
            return out;
        }

        // Snapshot the current damage as a sparse Int32Array of
        // [wordIdx, value, wordIdx, value, ...] pairs against the pristine
        // baseline. Returns null when no damage has accrued (fast path).
        function damageDiff() {
            if (!destructible || !damagedWords || damagedWords.size === 0) return null;
            const out = new Int32Array(damagedWords.size * 2);
            let i = 0;
            for (const [k, v] of damagedWords) { out[i++] = k; out[i++] = v | 0; }
            return out;
        }

        // Restore from a damageDiff. Resets bitmask to pristine first, then
        // applies the diff. damagedTiles is rebuilt from the resulting
        // damagedWords so the renderer routes the right tiles through the
        // scratch canvas on the next frame.
        function applyDamageDiff(diff) {
            if (!destructible) return;
            // Reset diverged words to pristine.
            for (const k of damagedWords.keys()) bitmask[k] = pristineMask[k];
            damagedWords.clear();
            pixelsCleared = 0;
            if (diff && diff.length) {
                for (let i = 0; i < diff.length; i += 2) {
                    const k = diff[i] | 0;
                    const v = (diff[i + 1] | 0) >>> 0;
                    bitmask[k] = v;
                    if (v !== pristineMask[k]) damagedWords.set(k, v);
                    // Count cleared bits relative to pristine for stats.
                    pixelsCleared += popcount(pristineMask[k] & ~v);
                }
            }
            rebuildDamagedTiles();
        }

        function popcount(v) {
            v = v - ((v >>> 1) & 0x55555555);
            v = (v & 0x33333333) + ((v >>> 2) & 0x33333333);
            return (((v + (v >>> 4)) & 0x0F0F0F0F) * 0x01010101) >>> 24;
        }

        // ── Overlay shapes (deferred-carve) ────────────────────────────────
        // Push circle/beam shapes onto the overlay list instead of carving
        // bitmask pixels. solidAtPixel honors them (treats covered pixels as
        // cleared). MCTS save/restore is just record/truncate the count.
        // commitOverlays bakes them into the bitmask in one pass — pixel
        // iteration cost is paid once per real fire instead of 24× / decision.

        // Read-only centerline ray-march. Returns { hit, hitX, hitY, len }
        // where (hitX, hitY) is the first solid-pixel impact (overlay-aware
        // via solidAtPixel) along the segment, or the segment endpoint if no
        // hit. Cheap (~len reads) — used by fireOneBeam to size the overlay.
        function traceBeam(x0, y0, x1, y1) {
            const dx = x1 - x0, dy = y1 - y0;
            const len = Math.hypot(dx, dy);
            if (len < 1) return { hit: false, hitX: x0, hitY: y0, len: 0 };
            const ux = dx / len, uy = dy / len;
            for (let s = 0; s <= len; s++) {
                const cx = x0 + ux * s;
                const cy = y0 + uy * s;
                if (solidAtPixel(cx, cy)) {
                    return { hit: true, hitX: cx, hitY: cy, len: s };
                }
            }
            return { hit: false, hitX: x1, hitY: y1, len };
        }

        function pushOverlayCircle(cx, cy, r) {
            if (!destructible) return;
            // Reuse pooled entries if present; otherwise allocate.
            let ov = overlays[overlayCount];
            if (!ov) { ov = {}; overlays.push(ov); }
            ov.kind = 0;
            ov.cx = cx; ov.cy = cy; ov.r2 = r * r; ov.r = r;
            overlayCount++;
        }

        function pushOverlayBeam(x0, y0, x1, y1, thickness) {
            if (!destructible) return;
            const dx = x1 - x0, dy = y1 - y0;
            const len = Math.hypot(dx, dy);
            if (len < 1) return;
            let ov = overlays[overlayCount];
            if (!ov) { ov = {}; overlays.push(ov); }
            ov.kind = 1;
            ov.x0 = x0; ov.y0 = y0;
            ov.ux = dx / len; ov.uy = dy / len;
            ov.nx = -ov.uy;   ov.ny = ov.ux;
            ov.len = len;
            ov.half = Math.max(1, thickness * 0.5);
            ov.thickness = thickness;
            overlayCount++;
        }

        // O(1). Used by MCTS env.snapshot — record where to truncate back to.
        function saveOverlaySnapshot() { overlaySaveCount = overlayCount; }
        // O(1). Used by MCTS env.restore — drop everything pushed since save.
        function restoreOverlaySnapshot() { overlayCount = overlaySaveCount; }
        function getOverlayCount() { return overlayCount; }

        // Commit: bake current overlays into the bitmask using the existing
        // pixel-precise damageBeam/damageCircle (which clear bits and update
        // damagedWords for damageDiff). Returns total pixels actually cleared
        // (sum of newly-flipped bits) so the caller can compute reward. This
        // is the ONE place pixel iteration happens for fires.
        function commitOverlays() {
            if (!destructible || overlayCount === 0) return 0;
            let total = 0;
            for (let i = 0; i < overlayCount; i++) {
                const ov = overlays[i];
                if (ov.kind === 0) {
                    total += damageCircle(ov.cx, ov.cy, ov.r) | 0;
                } else {
                    const x1 = ov.x0 + ov.ux * ov.len;
                    const y1 = ov.y0 + ov.uy * ov.len;
                    const r = damageBeam(ov.x0, ov.y0, x1, y1, ov.thickness, false);
                    total += r.cleared | 0;
                }
            }
            overlayCount = 0;
            overlaySaveCount = 0;
            return total;
        }

        function clearOverlays() { overlayCount = 0; overlaySaveCount = 0; }

        // Backwards-compat aliases for existing call sites in play_agent /
        // workers — same names, overlay-backed semantics.
        function saveDamageSnapshot()    { saveOverlaySnapshot(); }
        function restoreDamageSnapshot() { restoreOverlaySnapshot(); }
        function clearDamageSnapshot()   { clearOverlays(); }

        // Reset all damage to pristine. Used on episode reset.
        function resetDamage() {
            if (!destructible) return;
            for (const k of damagedWords.keys()) bitmask[k] = pristineMask[k];
            damagedWords.clear();
            if (damagedTiles) damagedTiles.clear();
            pixelsCleared = 0;
            overlayCount = 0;
            overlaySaveCount = 0;
        }

        // ── Drawing ────────────────────────────────────────────────────────

        // Stamp visible tiles per frame. In destructible mode, tiles whose
        // pristine pixels have been carved are routed through a tileSize
        // scratch canvas: drawTile into scratch, erase cleared pixel rows
        // with destination-out, then drawImage(scratch, ...) onto ctx. The
        // scratch is a small (tileSize × tileSize) offscreen canvas reused
        // across tiles — the same single-hop atlas-style pattern that
        // already works in bro's canvas pipeline.
        function draw(ctx, camX, camY, viewW, viewH) {
            if (!drawTile) return;
            const c0 = Math.max(0, Math.floor(camX / tileSize));
            const r0 = Math.max(0, Math.floor(camY / tileSize));
            const c1 = Math.min(cols - 1, Math.ceil((camX + viewW) / tileSize));
            const r1 = Math.min(rows - 1, Math.ceil((camY + viewH) / tileSize));
            const useScratch = destructible && damagedTiles && damagedTiles.size > 0
                            && typeof document !== 'undefined';
            if (useScratch) ensureScratch();
            for (let r = r0; r <= r1; r++) {
                for (let c = c0; c <= c1; c++) {
                    const id = data[idx(c, r)];
                    if (!id) continue;
                    const dx = c * tileSize - camX;
                    const dy = r * tileSize - camY;
                    if (useScratch && scratchCtx && damagedTiles.has(r * cols + c)) {
                        drawDamagedTile(ctx, c, r, id, dx, dy);
                    } else {
                        drawTile(ctx, id, dx, dy, tileSize);
                    }
                }
            }
        }

        function drawDamagedTile(ctx, c, r, id, dx, dy) {
            scratchCtx.clearRect(0, 0, tileSize, tileSize);
            drawTile(scratchCtx, id, 0, 0, tileSize);
            scratchCtx.save();
            scratchCtx.globalCompositeOperation = 'destination-out';
            scratchCtx.fillStyle = '#000';
            const tx0 = c * tileSize, ty0 = r * tileSize;
            for (let py = 0; py < tileSize; py++) {
                const wy = ty0 + py;
                if (wy < 0 || wy >= heightPx) continue;
                const rowBase = wy * wordsPerRow;
                let runStart = -1;
                for (let bx = 0; bx < tileSize; bx++) {
                    const xx = tx0 + bx;
                    let cleared = false;
                    if (xx >= 0 && xx < widthPx) {
                        const wIdx = rowBase + (xx >> 5);
                        const bit = 1 << (xx & 31);
                        cleared = (pristineMask[wIdx] & bit) !== 0
                               && (bitmask[wIdx] & bit) === 0;
                    }
                    if (cleared) {
                        if (runStart < 0) runStart = bx;
                    } else if (runStart >= 0) {
                        scratchCtx.fillRect(runStart, py, bx - runStart, 1);
                        runStart = -1;
                    }
                }
                if (runStart >= 0) {
                    scratchCtx.fillRect(runStart, py, tileSize - runStart, 1);
                }
            }
            scratchCtx.restore();
            ctx.drawImage(scratchCanvas, 0, 0, tileSize, tileSize,
                          dx, dy, tileSize, tileSize);
        }

        if (destructible) buildBitmask();

        return {
            tileSize, cols, rows, data,
            widthPx, heightPx,
            destructible,
            get, set, setRows,
            solidAt, solidAtPx, solidAtPixel,
            draw,
            damageBeam, damageCircle, traceBeam,
            damageDiff, applyDamageDiff, resetDamage,
            saveDamageSnapshot, restoreDamageSnapshot, clearDamageSnapshot,
            pushOverlayCircle, pushOverlayBeam,
            saveOverlaySnapshot, restoreOverlaySnapshot,
            commitOverlays, clearOverlays, getOverlayCount,
            get pixelsCleared() { return pixelsCleared; },
            get damagedWordCount() { return damagedWords ? damagedWords.size : 0; },
            setSolid(id, isSolid) { solid[id] = isSolid ? 1 : 0; },
            setDrawTile(fn) { drawTile = fn; },
        };
    }

    global.Tilemap = { create };
})(typeof window !== 'undefined' ? window : globalThis);
