// render.js — 3D vector projection + line-drawing primitives.
//
// Coordinate system:
//   +X right, +Y up, +Z forward (into the screen).
// The camera sits at the origin, looking toward +Z. World objects are
// positioned in camera-relative space (the player craft advances forward
// by translating world objects backward each frame on a rail).
var N = N || {};

N.Render = (function() {
    "use strict";

    // Field-of-view is a vertical FOV scaling factor: projected_y_px = H/2 - (y/z) * focal.
    // focal = (H/2) / tan(vfov/2). We target a ~60° vertical FOV for cinematic readability.
    var VFOV = 60 * Math.PI / 180;

    // Near-plane clip (world units). Behind this we cull entirely.
    var NEAR_Z = 0.5;

    // Current screen dims / derived focal; recomputed per frame via setViewport().
    var _W = 1024, _H = 768;
    var _focal = (_H / 2) / Math.tan(VFOV / 2);
    var _cx = _W / 2, _cy = _H / 2;

    // Screen-shake & vector jitter (driven by game.js on damage events).
    var _shakeAmp = 0;   // px
    var _shakeDecay = 0; // per ms
    var _jitter = 0;     // px, line endpoint noise

    // Parallax camera offset in world units. World points are translated by
    // (-_camX, -_camY) before projection — the arcade sells "looking around"
    // by sliding the view slightly opposite the yoke. Reticle/HUD uses
    // projectHud() to stay pinned to screen space.
    var _camX = 0, _camY = 0;

    function setCamera(cx, cy) { _camX = cx || 0; _camY = cy || 0; }

    function setViewport(W, H) {
        _W = W; _H = H;
        _focal = (H / 2) / Math.tan(VFOV / 2);
        _cx = W / 2; _cy = H / 2;
    }

    function focal() { return _focal; }
    function width()  { return _W; }
    function height() { return _H; }

    // Project a camera-space point to screen pixels. Returns {x,y,z,visible}.
    // visible=false when behind the near plane. x/y are screen pixels, z is
    // world-space depth (kept for sorting and depth-fade).
    function project(x, y, z) {
        if (z < NEAR_Z) return { x: 0, y: 0, z: z, visible: false };
        var inv = _focal / z;
        var sx = _cx + (x - _camX) * inv + (_shakeAmp ? (Math.random() * 2 - 1) * _shakeAmp : 0);
        var sy = _cy - (y - _camY) * inv + (_shakeAmp ? (Math.random() * 2 - 1) * _shakeAmp : 0);
        return { x: sx, y: sy, z: z, visible: true };
    }

    // HUD projection — ignores camera parallax. Use for reticle and any
    // element that should stay attached to the cockpit, not the world.
    function projectHud(x, y, z) {
        if (z < NEAR_Z) return { x: 0, y: 0, z: z, visible: false };
        var inv = _focal / z;
        var sx = _cx + x * inv;
        var sy = _cy - y * inv;
        return { x: sx, y: sy, z: z, visible: true };
    }

    // Depth-fade: 1 at near, 0 at far. Used to attenuate stroke alpha so
    // distant geometry recedes rather than cutting off sharply.
    function depthFade(z, far) {
        far = far || 400;
        if (z < NEAR_Z) return 0;
        if (z > far) return 0;
        return 1 - (z / far);
    }

    // Draw a line between two camera-space points. Handles near-plane clip
    // by interpolating. Returns true if the segment was drawn.
    function line(ctx, ax, ay, az, bx, by, bz, color, alpha) {
        // Near-plane clip: if both behind, skip. If one behind, interpolate.
        if (az < NEAR_Z && bz < NEAR_Z) return false;
        if (az < NEAR_Z) {
            var t = (NEAR_Z - az) / (bz - az);
            ax = ax + (bx - ax) * t;
            ay = ay + (by - ay) * t;
            az = NEAR_Z;
        } else if (bz < NEAR_Z) {
            var t2 = (NEAR_Z - bz) / (az - bz);
            bx = bx + (ax - bx) * t2;
            by = by + (ay - by) * t2;
            bz = NEAR_Z;
        }
        var pa = project(ax, ay, az);
        var pb = project(bx, by, bz);
        if (!pa.visible || !pb.visible) return false;
        ctx.strokeStyle = color;
        ctx.globalAlpha = (alpha != null) ? alpha : 1;
        ctx.beginPath();
        var jx = _jitter ? (Math.random() * 2 - 1) * _jitter : 0;
        var jy = _jitter ? (Math.random() * 2 - 1) * _jitter : 0;
        ctx.moveTo(pa.x + jx, pa.y + jy);
        jx = _jitter ? (Math.random() * 2 - 1) * _jitter : 0;
        jy = _jitter ? (Math.random() * 2 - 1) * _jitter : 0;
        ctx.lineTo(pb.x + jx, pb.y + jy);
        ctx.stroke();
        ctx.globalAlpha = 1;
        return true;
    }

    // Draw a polyline defined by world points (array of {x,y,z}), optionally
    // transformed by {ox,oy,oz} offset and {sx,sy,sz} scale.
    function polyline(ctx, pts, close, color, alpha, transform) {
        var t = transform || {};
        var ox = t.ox || 0, oy = t.oy || 0, oz = t.oz || 0;
        var sx = t.sx != null ? t.sx : 1, sy = t.sy != null ? t.sy : 1, sz = t.sz != null ? t.sz : 1;
        for (var i = 0; i < pts.length - 1; i++) {
            var a = pts[i], b = pts[i + 1];
            line(ctx, a.x * sx + ox, a.y * sy + oy, a.z * sz + oz,
                      b.x * sx + ox, b.y * sy + oy, b.z * sz + oz, color, alpha);
        }
        if (close && pts.length > 1) {
            var a2 = pts[pts.length - 1], b2 = pts[0];
            line(ctx, a2.x * sx + ox, a2.y * sy + oy, a2.z * sz + oz,
                      b2.x * sx + ox, b2.y * sy + oy, b2.z * sz + oz, color, alpha);
        }
    }

    // Draw a list of edges [[i,j], ...] into a mesh of vertices.
    function edges(ctx, verts, edgeList, color, alpha, transform) {
        var t = transform || {};
        var ox = t.ox || 0, oy = t.oy || 0, oz = t.oz || 0;
        var sx = t.sx != null ? t.sx : 1, sy = t.sy != null ? t.sy : 1, sz = t.sz != null ? t.sz : 1;
        for (var i = 0; i < edgeList.length; i++) {
            var e = edgeList[i];
            var a = verts[e[0]], b = verts[e[1]];
            line(ctx, a.x * sx + ox, a.y * sy + oy, a.z * sz + oz,
                      b.x * sx + ox, b.y * sy + oy, b.z * sz + oz, color, alpha);
        }
    }

    // --- Starfield ---------------------------------------------------------
    // A pseudo-3D starfield: stars are 3D points in front of the camera,
    // advanced by dz each frame. When a star passes the camera, it respawns
    // far away.
    var _stars = [];
    var STAR_COUNT = 140;
    var STAR_FAR = 400;

    function initStars() {
        _stars.length = 0;
        for (var i = 0; i < STAR_COUNT; i++) {
            _stars.push({
                x: (Math.random() * 2 - 1) * 300,
                y: (Math.random() * 2 - 1) * 220,
                z: NEAR_Z + Math.random() * STAR_FAR
            });
        }
    }

    function advanceStars(dz) {
        for (var i = 0; i < _stars.length; i++) {
            var s = _stars[i];
            s.z -= dz;
            if (s.z < NEAR_Z + 1) {
                s.x = (Math.random() * 2 - 1) * 300;
                s.y = (Math.random() * 2 - 1) * 220;
                s.z = STAR_FAR;
            }
        }
    }

    function drawStars(ctx) {
        ctx.fillStyle = "#ffffff";
        for (var i = 0; i < _stars.length; i++) {
            var s = _stars[i];
            var p = project(s.x, s.y, s.z);
            if (!p.visible) continue;
            var fade = depthFade(s.z, STAR_FAR);
            if (fade < 0.05) continue;
            ctx.globalAlpha = fade;
            var sz = 1 + (1 - s.z / STAR_FAR) * 1.5;
            ctx.fillRect(p.x | 0, p.y | 0, sz, sz);
        }
        ctx.globalAlpha = 1;
    }

    // --- Screen shake / jitter control ------------------------------------

    function shake(amount, decayMs) {
        _shakeAmp = Math.max(_shakeAmp, amount);
        _shakeDecay = (decayMs || 250);
    }

    function setJitter(px) { _jitter = px; }

    function updateShake(dt) {
        if (_shakeAmp > 0) {
            _shakeAmp -= _shakeAmp * Math.min(1, dt / _shakeDecay);
            if (_shakeAmp < 0.1) _shakeAmp = 0;
        }
    }

    // --- Full-screen flash (damage feedback) ------------------------------
    var _flashColor = null;
    var _flashT = 0;
    var _flashDur = 0;

    function flash(color, ms) {
        _flashColor = color;
        _flashT = 0;
        _flashDur = ms;
    }

    function drawFlash(ctx) {
        if (!_flashColor || _flashT >= _flashDur) return;
        var a = 1 - _flashT / _flashDur;
        ctx.fillStyle = _flashColor;
        ctx.globalAlpha = a * 0.35;
        ctx.fillRect(0, 0, _W, _H);
        ctx.globalAlpha = 1;
    }

    function updateFlash(dt) {
        if (_flashColor) {
            _flashT += dt;
            if (_flashT >= _flashDur) _flashColor = null;
        }
    }

    return {
        setViewport: setViewport,
        width: width, height: height, focal: focal,
        project: project,
        projectHud: projectHud,
        setCamera: setCamera,
        line: line,
        polyline: polyline,
        edges: edges,
        depthFade: depthFade,
        initStars: initStars,
        advanceStars: advanceStars,
        drawStars: drawStars,
        shake: shake,
        setJitter: setJitter,
        updateShake: updateShake,
        flash: flash,
        drawFlash: drawFlash,
        updateFlash: updateFlash,
        NEAR_Z: NEAR_Z
    };
})();
