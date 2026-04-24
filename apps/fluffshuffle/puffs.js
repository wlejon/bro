// puffs.js — procedural rendering of fluffy creature tiles ("puffs").
//
// A puff is a colored blob with a jittered fur outline, two eyes that track
// the cursor, and a simple mouth that reflects state (happy/scared/popping).
// No external images; shapes are parameterized by color index so each
// variety reads as its own character even to colorblind players.
'use strict';
var G = G || {};

G.Puffs = (function () {

    // Core palette — six distinct puff flavors.
    var PALETTE = [
        null,
        { name: 'ember',    core: '#ff6e7a', belly: '#ffc0c6', dark: '#a02030', outline: '#5a0f1f', fur: 1.00, bumps: 14 },
        { name: 'tide',     core: '#4fa8ff', belly: '#b8d8ff', dark: '#153a78', outline: '#0a1a44', fur: 0.70, bumps: 10 },
        { name: 'mint',     core: '#5ed59a', belly: '#bff0d2', dark: '#166a3f', outline: '#073320', fur: 1.30, bumps: 18 },
        { name: 'daisy',    core: '#ffcc54', belly: '#fff0b0', dark: '#8a6410', outline: '#4a3208', fur: 0.55, bumps: 8  },
        { name: 'dusk',     core: '#c78aff', belly: '#e6c8ff', dark: '#5a2a88', outline: '#2a1050', fur: 1.10, bumps: 12 },
        { name: 'cocoa',    core: '#b07050', belly: '#e0b090', dark: '#4a2810', outline: '#1a0c04', fur: 1.60, bumps: 20 },
    ];

    // Eye position / mouth style per color, so each puff looks like a slightly
    // different species (accessibility + character).
    var SHAPE = [
        null,
        { eyeSpread: 0.38, eyeY: -0.12, eyeR: 0.14, mouthY: 0.24, mouthW: 0.32, mouthShape: 'smile' },
        { eyeSpread: 0.34, eyeY: -0.14, eyeR: 0.12, mouthY: 0.20, mouthW: 0.26, mouthShape: 'smile' },
        { eyeSpread: 0.42, eyeY: -0.10, eyeR: 0.16, mouthY: 0.22, mouthW: 0.38, mouthShape: 'grin'  },
        { eyeSpread: 0.30, eyeY: -0.16, eyeR: 0.11, mouthY: 0.20, mouthW: 0.22, mouthShape: 'dot'   },
        { eyeSpread: 0.36, eyeY: -0.12, eyeR: 0.14, mouthY: 0.22, mouthW: 0.28, mouthShape: 'oh'    },
        { eyeSpread: 0.40, eyeY: -0.10, eyeR: 0.15, mouthY: 0.26, mouthW: 0.30, mouthShape: 'smile' },
    ];

    // Draw a puff body at (cx, cy) sized `size`. `opts` is { pulse, squish,
    // lookAt:{x,y}, state: 'idle'|'held'|'scared'|'pop', t (ms) }.
    function draw(ctx, puff, cx, cy, size, opts) {
        if (!puff) return;
        opts = opts || {};
        var pal = PALETTE[puff.color];
        if (!pal) return;
        var shape = SHAPE[puff.color] || SHAPE[1];

        var scale = size * 0.46; // radius
        var pulse = opts.pulse || 0;
        var squish = opts.squish || 0;
        var t = opts.t || 0;
        var state = opts.state || 'idle';

        // Base radius + gentle idle breathing so the board feels alive.
        var breathe = Math.sin(t * 0.002 + puff.phase) * 0.025;
        var rx = scale * (1 + breathe + pulse * 0.10 + squish);
        var ry = scale * (1 + breathe + pulse * 0.10 - squish);

        // Held puff: bulge + slight upward shift.
        if (state === 'held') {
            rx *= 1.10; ry *= 1.10;
            cy -= scale * 0.04;
        }
        if (state === 'pop') {
            rx *= (1 + opts.popProgress * 0.5);
            ry *= (1 + opts.popProgress * 0.5);
        }

        ctx.save();
        ctx.translate(cx, cy);

        // Subtle shadow beneath (reads as depth).
        ctx.globalAlpha = 0.28 * (1 - (opts.popProgress || 0));
        ctx.fillStyle = '#000';
        ctx.beginPath();
        ctx.ellipse(0, ry * 0.85, rx * 0.75, ry * 0.18, 0, 0, Math.PI * 2);
        ctx.fill();
        ctx.globalAlpha = 1;

        // Fur silhouette: an ellipse perimeter deformed by sinusoidal bumps
        // plus per-puff random jitter. This reads as fuzzy fur even when
        // small, without any raster texture.
        var bumps = shape ? (SHAPE[puff.color].bumps || 12) : 12;
        var furAmp = pal.fur * scale * 0.09;
        var segs = Math.max(24, bumps * 3);
        ctx.beginPath();
        for (var i = 0; i <= segs; i++) {
            var a = (i / segs) * Math.PI * 2;
            var jitter = Math.sin(a * bumps + puff.phase) * furAmp
                       + Math.sin(a * (bumps * 1.7) + puff.phase * 1.3) * furAmp * 0.35;
            var r = 1 + jitter / scale;
            var x = Math.cos(a) * rx * r;
            var y = Math.sin(a) * ry * r;
            if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
        }
        ctx.closePath();

        var grad = ctx.createRadialGradient(-rx * 0.25, -ry * 0.35, rx * 0.1, 0, 0, rx * 1.1);
        grad.addColorStop(0, pal.belly);
        grad.addColorStop(0.55, pal.core);
        grad.addColorStop(1, pal.dark);
        ctx.fillStyle = grad;
        ctx.fill();

        ctx.lineWidth = 1.5;
        ctx.strokeStyle = pal.outline;
        ctx.stroke();

        // Belly patch — a softer oval on the lower half so the character
        // reads as a fluffy creature rather than a coin.
        ctx.beginPath();
        ctx.ellipse(0, ry * 0.25, rx * 0.55, ry * 0.40, 0, 0, Math.PI * 2);
        ctx.fillStyle = hexWithAlpha(pal.belly, 0.65);
        ctx.fill();

        // Eyes — two white orbs with pupils that track cursor.
        var ex = rx * shape.eyeSpread;
        var ey = ry * shape.eyeY;
        var er = scale * shape.eyeR;
        // Blink envelope.
        var blinkT = ((t + puff.blinkOffset) % 3200) / 3200;
        var blink = blinkT > 0.95 ? (1 - (blinkT - 0.95) / 0.05 * 2)
                    : blinkT > 0.90 ? ((blinkT - 0.90) / 0.05 * 2)
                    : 1;
        if (blink < 0) blink = 0;

        drawEye(ctx, -ex, ey, er, blink, opts, cx, cy);
        drawEye(ctx,  ex, ey, er, blink, opts, cx, cy);

        // Mouth.
        var my = ry * shape.mouthY;
        var mw = rx * shape.mouthW;
        var mouth = state === 'pop' ? 'oh' :
                    state === 'scared' ? 'oh' :
                    shape.mouthShape;
        drawMouth(ctx, 0, my, mw, mouth, pal.outline);

        // Locked overlay: a wrapping clamp glyph.
        if (puff.locked) {
            ctx.save();
            ctx.strokeStyle = 'rgba(180, 220, 255, 0.85)';
            ctx.fillStyle = 'rgba(180, 220, 255, 0.12)';
            ctx.lineWidth = 2;
            // Clamp: crescents at top and bottom.
            ctx.beginPath();
            ctx.arc(0, -ry * 0.05, rx * 0.90, -Math.PI * 0.75, -Math.PI * 0.25);
            ctx.stroke();
            ctx.beginPath();
            ctx.arc(0, -ry * 0.05, rx * 0.90, Math.PI * 0.25, Math.PI * 0.75);
            ctx.stroke();
            // Screw in middle.
            ctx.beginPath();
            ctx.arc(rx * 0.85, -ry * 0.05, scale * 0.08, 0, Math.PI * 2);
            ctx.fillStyle = 'rgba(220, 240, 255, 0.85)';
            ctx.fill();
            ctx.restore();
        }

        // Specials glyph overlay.
        if (puff.special === 1) {
            // Jumbo — plus sign sparkle.
            ctx.strokeStyle = 'rgba(255, 255, 220, 0.9)';
            ctx.lineWidth = 2;
            ctx.beginPath();
            ctx.moveTo(-rx * 0.25, 0); ctx.lineTo(rx * 0.25, 0);
            ctx.moveTo(0, -ry * 0.25); ctx.lineTo(0, ry * 0.25);
            ctx.stroke();
        } else if (puff.special === 2) {
            // Arrow — double-headed along its orientation axis.
            ctx.strokeStyle = 'rgba(255, 255, 255, 0.9)';
            ctx.lineWidth = 2.4;
            ctx.beginPath();
            if (puff.arrowDir === 'h') {
                ctx.moveTo(-rx * 0.55, 0); ctx.lineTo(rx * 0.55, 0);
                ctx.moveTo(-rx * 0.55, 0); ctx.lineTo(-rx * 0.35, -rx * 0.15);
                ctx.moveTo(-rx * 0.55, 0); ctx.lineTo(-rx * 0.35,  rx * 0.15);
                ctx.moveTo( rx * 0.55, 0); ctx.lineTo( rx * 0.35, -rx * 0.15);
                ctx.moveTo( rx * 0.55, 0); ctx.lineTo( rx * 0.35,  rx * 0.15);
            } else {
                ctx.moveTo(0, -ry * 0.55); ctx.lineTo(0, ry * 0.55);
                ctx.moveTo(0, -ry * 0.55); ctx.lineTo(-ry * 0.15, -ry * 0.35);
                ctx.moveTo(0, -ry * 0.55); ctx.lineTo( ry * 0.15, -ry * 0.35);
                ctx.moveTo(0,  ry * 0.55); ctx.lineTo(-ry * 0.15,  ry * 0.35);
                ctx.moveTo(0,  ry * 0.55); ctx.lineTo( ry * 0.15,  ry * 0.35);
            }
            ctx.stroke();
        } else if (puff.special === 3) {
            // Prism — diamond core.
            ctx.beginPath();
            ctx.moveTo(0, -ry * 0.32);
            ctx.lineTo( rx * 0.32, 0);
            ctx.lineTo(0,  ry * 0.32);
            ctx.lineTo(-rx * 0.32, 0);
            ctx.closePath();
            var g2 = ctx.createLinearGradient(-rx * 0.3, -ry * 0.3, rx * 0.3, ry * 0.3);
            g2.addColorStop(0, '#ff8bd4');
            g2.addColorStop(0.5, '#ffe27a');
            g2.addColorStop(1, '#8be5ff');
            ctx.fillStyle = g2;
            ctx.fill();
            ctx.strokeStyle = '#fff';
            ctx.lineWidth = 1;
            ctx.stroke();
        }

        ctx.restore();
    }

    function drawEye(ctx, ex, ey, er, blink, opts, cx, cy) {
        ctx.save();
        if (blink <= 0.02) {
            ctx.strokeStyle = '#222';
            ctx.lineWidth = 1.6;
            ctx.beginPath();
            ctx.moveTo(ex - er, ey); ctx.lineTo(ex + er, ey);
            ctx.stroke();
            ctx.restore();
            return;
        }
        // White.
        ctx.fillStyle = '#fff';
        ctx.beginPath();
        ctx.ellipse(ex, ey, er, er * blink, 0, 0, Math.PI * 2);
        ctx.fill();
        ctx.strokeStyle = '#1a1a1a';
        ctx.lineWidth = 1;
        ctx.stroke();

        // Pupil tracks cursor in world space. `lookAt` is a {x,y} in the
        // same coords as cx,cy, so translate back out.
        var dx = 0, dy = 0;
        if (opts.lookAt) {
            var wx = cx + ex, wy = cy + ey;
            dx = opts.lookAt.x - wx;
            dy = opts.lookAt.y - wy;
            var len = Math.sqrt(dx * dx + dy * dy);
            if (len > 0.0001) {
                var maxOff = er * 0.45;
                var s = Math.min(1, len / 40);
                dx = dx / len * maxOff * s;
                dy = dy / len * maxOff * s * blink;
            }
        }
        ctx.fillStyle = '#111';
        ctx.beginPath();
        ctx.ellipse(ex + dx, ey + dy, er * 0.55, er * 0.55 * blink, 0, 0, Math.PI * 2);
        ctx.fill();
        // Glint.
        ctx.fillStyle = '#fff';
        ctx.beginPath();
        ctx.arc(ex + dx - er * 0.18, ey + dy - er * 0.25 * blink, er * 0.18, 0, Math.PI * 2);
        ctx.fill();
        ctx.restore();
    }

    function drawMouth(ctx, x, y, w, kind, color) {
        ctx.save();
        ctx.strokeStyle = color;
        ctx.lineWidth = 1.6;
        ctx.lineCap = 'round';
        if (kind === 'smile') {
            ctx.beginPath();
            ctx.arc(x, y - w * 0.20, w * 0.6, Math.PI * 0.15, Math.PI - Math.PI * 0.15);
            ctx.stroke();
        } else if (kind === 'grin') {
            ctx.beginPath();
            ctx.moveTo(x - w * 0.5, y);
            ctx.quadraticCurveTo(x, y + w * 0.45, x + w * 0.5, y);
            ctx.stroke();
            // tooth
            ctx.fillStyle = '#fff';
            ctx.fillRect(x - w * 0.10, y + w * 0.05, w * 0.20, w * 0.18);
        } else if (kind === 'dot') {
            ctx.fillStyle = color;
            ctx.beginPath();
            ctx.arc(x, y, w * 0.15, 0, Math.PI * 2);
            ctx.fill();
        } else if (kind === 'oh') {
            ctx.fillStyle = color;
            ctx.beginPath();
            ctx.ellipse(x, y, w * 0.22, w * 0.30, 0, 0, Math.PI * 2);
            ctx.fill();
        }
        ctx.restore();
    }

    function hexWithAlpha(hex, a) {
        var c = hex.replace('#', '');
        var r = parseInt(c.substring(0, 2), 16);
        var g = parseInt(c.substring(2, 4), 16);
        var b = parseInt(c.substring(4, 6), 16);
        return 'rgba(' + r + ',' + g + ',' + b + ',' + a + ')';
    }

    function palette() { return PALETTE; }
    function shape() { return SHAPE; }

    return { draw: draw, palette: palette, shape: shape, PALETTE: PALETTE };
})();
