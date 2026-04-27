// camera2d.js — 2D follow camera with deadzone + level-bounds clamping.
//
// The camera owns a viewport of size (viewW, viewH) inside a level of size
// (levelW, levelH). Calling follow(x, y) keeps the target inside a centered
// deadzone — the camera only moves once the target leaves it. The camera is
// then clamped to the level so it never reveals out-of-bounds area.
//
// Coordinate convention: cam.x / cam.y are the world-space top-left of the
// viewport. To draw world content, blit at (worldX - cam.x, worldY - cam.y).
//
// Usage:
//   <script src="../lib/camera2d.js"></script>
//   const cam = Camera2D.create({
//       viewW: 800, viewH: 576,
//       levelW: tm.widthPx, levelH: tm.heightPx,
//       deadzoneW: 160, deadzoneH: 96,
//   });
//   cam.follow(player.x + player.w / 2, player.y + player.h / 2);
//   tm.draw(ctx, cam.x, cam.y, cam.viewW, cam.viewH);

(function (global) {
    'use strict';

    function create(opts) {
        opts = opts || {};
        const cam = {
            x: 0, y: 0,
            viewW: opts.viewW || 800,
            viewH: opts.viewH || 600,
            levelW: opts.levelW || 0,
            levelH: opts.levelH || 0,
            deadzoneW: opts.deadzoneW != null ? opts.deadzoneW : 160,
            deadzoneH: opts.deadzoneH != null ? opts.deadzoneH : 96,
        };

        function clamp() {
            const maxX = Math.max(0, cam.levelW - cam.viewW);
            const maxY = Math.max(0, cam.levelH - cam.viewH);
            if (cam.x < 0) cam.x = 0;
            else if (cam.x > maxX) cam.x = maxX;
            if (cam.y < 0) cam.y = 0;
            else if (cam.y > maxY) cam.y = maxY;
        }

        cam.follow = function (tx, ty) {
            // Push camera only when target leaves the centered deadzone.
            const cx = cam.x + cam.viewW / 2;
            const cy = cam.y + cam.viewH / 2;
            const dx = tx - cx;
            const dy = ty - cy;
            const hzW = cam.deadzoneW / 2;
            const hzH = cam.deadzoneH / 2;
            if (dx >  hzW) cam.x += dx - hzW;
            if (dx < -hzW) cam.x += dx + hzW;
            if (dy >  hzH) cam.y += dy - hzH;
            if (dy < -hzH) cam.y += dy + hzH;
            clamp();
        };

        cam.snapTo = function (tx, ty) {
            cam.x = tx - cam.viewW / 2;
            cam.y = ty - cam.viewH / 2;
            clamp();
        };

        cam.setLevelSize = function (w, h) {
            cam.levelW = w; cam.levelH = h; clamp();
        };

        cam.worldToScreen = function (wx, wy) {
            return { x: wx - cam.x, y: wy - cam.y };
        };

        cam.visible = function (wx, wy, w, h) {
            return wx + w  > cam.x && wx < cam.x + cam.viewW
                && wy + h  > cam.y && wy < cam.y + cam.viewH;
        };

        return cam;
    }

    global.Camera2D = { create };
})(typeof window !== 'undefined' ? window : globalThis);
