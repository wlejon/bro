// Drawing helpers tuned for pixel-art sprite/tile authoring.
// All helpers operate on a CanvasRenderingContext2D whose
// imageSmoothingEnabled is false (set by the caller in render()).

const brush = (() => {

    // ---- Palettes (small, hand-tuned, easy to extend) -------------------

    // PICO-8-style 16-color palette — good general-purpose starting point.
    const PICO8 = [
        '#000000','#1D2B53','#7E2553','#008751',
        '#AB5236','#5F574F','#C2C3C7','#FFF1E8',
        '#FF004D','#FFA300','#FFEC27','#00E436',
        '#29ADFF','#83769C','#FF77A8','#FFCCAA'
    ];

    // Endesga 16 — warmer, more nuanced.
    const ENDESGA16 = [
        '#e4a672','#b86f50','#743f39','#3f2832',
        '#9e2835','#e53b44','#fb922b','#ffe762',
        '#63c64d','#327345','#193d3f','#4f6781',
        '#afbfd2','#ffffff','#2ce8f4','#0484d1'
    ];

    // ---- Pixel primitives (rect-fill so they survive integer scaling) ----

    function px(ctx, x, y, color) {
        if (color !== undefined) ctx.fillStyle = color;
        ctx.fillRect(x | 0, y | 0, 1, 1);
    }

    function hline(ctx, x, y, len, color) {
        if (color !== undefined) ctx.fillStyle = color;
        ctx.fillRect(x | 0, y | 0, len | 0, 1);
    }

    function vline(ctx, x, y, len, color) {
        if (color !== undefined) ctx.fillStyle = color;
        ctx.fillRect(x | 0, y | 0, 1, len | 0);
    }

    function rect(ctx, x, y, w, h, color) {
        if (color !== undefined) ctx.fillStyle = color;
        ctx.fillRect(x | 0, y | 0, w | 0, h | 0);
    }

    function rectOutline(ctx, x, y, w, h, color) {
        if (color !== undefined) ctx.fillStyle = color;
        x|=0; y|=0; w|=0; h|=0;
        ctx.fillRect(x, y, w, 1);
        ctx.fillRect(x, y + h - 1, w, 1);
        ctx.fillRect(x, y, 1, h);
        ctx.fillRect(x + w - 1, y, 1, h);
    }

    // Bresenham line in pixel-perfect cells.
    function line(ctx, x0, y0, x1, y1, color) {
        if (color !== undefined) ctx.fillStyle = color;
        x0|=0; y0|=0; x1|=0; y1|=0;
        const dx = Math.abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
        const dy = -Math.abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
        let err = dx + dy;
        for (;;) {
            ctx.fillRect(x0, y0, 1, 1);
            if (x0 === x1 && y0 === y1) break;
            const e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }

    // Filled circle (midpoint algorithm). Center (cx,cy), radius r.
    function circle(ctx, cx, cy, r, color) {
        if (color !== undefined) ctx.fillStyle = color;
        cx|=0; cy|=0; r|=0;
        for (let dy = -r; dy <= r; dy++) {
            const w = Math.floor(Math.sqrt(r * r - dy * dy));
            ctx.fillRect(cx - w, cy + dy, 2 * w + 1, 1);
        }
    }

    function circleOutline(ctx, cx, cy, r, color) {
        if (color !== undefined) ctx.fillStyle = color;
        cx|=0; cy|=0; r|=0;
        let x = r, y = 0, err = 1 - r;
        while (x >= y) {
            ctx.fillRect(cx + x, cy + y, 1, 1);
            ctx.fillRect(cx + y, cy + x, 1, 1);
            ctx.fillRect(cx - y, cy + x, 1, 1);
            ctx.fillRect(cx - x, cy + y, 1, 1);
            ctx.fillRect(cx - x, cy - y, 1, 1);
            ctx.fillRect(cx - y, cy - x, 1, 1);
            ctx.fillRect(cx + y, cy - x, 1, 1);
            ctx.fillRect(cx + x, cy - y, 1, 1);
            y++;
            if (err < 0) err += 2 * y + 1;
            else { x--; err += 2 * (y - x) + 1; }
        }
    }

    // Paint from a tiny ASCII grid. '.' = transparent, ' ' = transparent,
    // any other char looks up `palette[char]`.  Origin is (x, y) top-left.
    //
    //   brush.stamp(ctx, 0, 0, [
    //       '..XX..',
    //       '.XOOX.',
    //       'XOOOOX',
    //   ], { X: '#000', O: '#fc0' });
    function stamp(ctx, x, y, rows, palette) {
        x|=0; y|=0;
        for (let r = 0; r < rows.length; r++) {
            const row = rows[r];
            for (let c = 0; c < row.length; c++) {
                const ch = row[c];
                if (ch === '.' || ch === ' ') continue;
                const col = palette[ch];
                if (!col) continue;
                ctx.fillStyle = col;
                ctx.fillRect(x + c, y + r, 1, 1);
            }
        }
    }

    // Vertical gradient over a rect, color-quantized to `steps` bands.
    // Picks colors by lerping between hex `topHex` and `botHex`.
    function gradV(ctx, x, y, w, h, topHex, botHex, steps) {
        x|=0; y|=0; w|=0; h|=0;
        steps = Math.max(1, steps | 0);
        const t = hexToRgb(topHex), b = hexToRgb(botHex);
        for (let i = 0; i < steps; i++) {
            const k = steps === 1 ? 0 : i / (steps - 1);
            const r = Math.round(t.r + (b.r - t.r) * k);
            const g = Math.round(t.g + (b.g - t.g) * k);
            const bl = Math.round(t.b + (b.b - t.b) * k);
            ctx.fillStyle = `rgb(${r},${g},${bl})`;
            const y0 = y + Math.floor(i * h / steps);
            const y1 = y + Math.floor((i + 1) * h / steps);
            ctx.fillRect(x, y0, w, y1 - y0);
        }
    }

    function hexToRgb(hex) {
        const s = hex.replace('#','');
        const n = parseInt(s, 16);
        if (s.length === 6) return { r:(n>>16)&255, g:(n>>8)&255, b:n&255 };
        if (s.length === 3) {
            const r=(n>>8)&15, g=(n>>4)&15, b=n&15;
            return { r:r*17, g:g*17, b:b*17 };
        }
        return { r:0,g:0,b:0 };
    }

    // Mirror the left half of a (w, h) region to the right half — handy for
    // building symmetric characters: draw the left side, then `mirror(...)`.
    // This uses getImageData/putImageData; falls back silently on contexts
    // that don't support it.
    function mirrorH(ctx, x, y, w, h) {
        try {
            const half = Math.floor(w / 2);
            const src = ctx.getImageData(x, y, half, h);
            const tmp = ctx.createImageData(half, h);
            for (let py = 0; py < h; py++) {
                for (let px = 0; px < half; px++) {
                    const sIdx = (py * half + px) * 4;
                    const dIdx = (py * half + (half - 1 - px)) * 4;
                    tmp.data[dIdx]     = src.data[sIdx];
                    tmp.data[dIdx + 1] = src.data[sIdx + 1];
                    tmp.data[dIdx + 2] = src.data[sIdx + 2];
                    tmp.data[dIdx + 3] = src.data[sIdx + 3];
                }
            }
            ctx.putImageData(tmp, x + (w - half), y);
        } catch (e) { /* canvas2D scene path may not support imageData yet */ }
    }

    // ---- Smooth-mode helpers (antialiased; assume imageSmoothingEnabled) -

    const smooth = {
        roundRect(ctx, x, y, w, h, r, fill) {
            if (fill !== undefined) ctx.fillStyle = fill;
            r = Math.min(r, w / 2, h / 2);
            if (r <= 0) { ctx.fillRect(x, y, w, h); return; }
            ctx.beginPath();
            ctx.moveTo(x + r, y);
            ctx.lineTo(x + w - r, y);
            ctx.quadraticCurveTo(x + w, y, x + w, y + r);
            ctx.lineTo(x + w, y + h - r);
            ctx.quadraticCurveTo(x + w, y + h, x + w - r, y + h);
            ctx.lineTo(x + r, y + h);
            ctx.quadraticCurveTo(x, y + h, x, y + h - r);
            ctx.lineTo(x, y + r);
            ctx.quadraticCurveTo(x, y, x + r, y);
            ctx.closePath();
            ctx.fill();
        },
        roundRectOutline(ctx, x, y, w, h, r, stroke, lineWidth) {
            if (stroke !== undefined) ctx.strokeStyle = stroke;
            if (lineWidth !== undefined) ctx.lineWidth = lineWidth;
            r = Math.min(r, w / 2, h / 2);
            ctx.beginPath();
            if (r <= 0) { ctx.rect(x, y, w, h); ctx.stroke(); return; }
            ctx.moveTo(x + r, y);
            ctx.lineTo(x + w - r, y);
            ctx.quadraticCurveTo(x + w, y, x + w, y + r);
            ctx.lineTo(x + w, y + h - r);
            ctx.quadraticCurveTo(x + w, y + h, x + w - r, y + h);
            ctx.lineTo(x + r, y + h);
            ctx.quadraticCurveTo(x, y + h, x, y + h - r);
            ctx.lineTo(x, y + r);
            ctx.quadraticCurveTo(x, y, x + r, y);
            ctx.closePath();
            ctx.stroke();
        },
        circle(ctx, cx, cy, r, fill) {
            if (fill !== undefined) ctx.fillStyle = fill;
            ctx.beginPath();
            ctx.arc(cx, cy, r, 0, Math.PI * 2);
            ctx.fill();
        },
        circleOutline(ctx, cx, cy, r, stroke, lineWidth) {
            if (stroke !== undefined) ctx.strokeStyle = stroke;
            if (lineWidth !== undefined) ctx.lineWidth = lineWidth;
            ctx.beginPath();
            ctx.arc(cx, cy, r, 0, Math.PI * 2);
            ctx.stroke();
        },
        ellipse(ctx, cx, cy, rx, ry, rotation, fill) {
            if (fill !== undefined) ctx.fillStyle = fill;
            ctx.beginPath();
            ctx.ellipse(cx, cy, rx, ry, rotation || 0, 0, Math.PI * 2);
            ctx.fill();
        },
        linearGradient(ctx, x0, y0, x1, y1, stops) {
            const g = ctx.createLinearGradient(x0, y0, x1, y1);
            for (const [t, col] of stops) g.addColorStop(t, col);
            return g;
        },
        radialGradient(ctx, cx, cy, r0, r1, stops) {
            const g = ctx.createRadialGradient(cx, cy, r0, cx, cy, r1);
            for (const [t, col] of stops) g.addColorStop(t, col);
            return g;
        },
        polyline(ctx, points, close) {
            if (!points.length) return;
            ctx.beginPath();
            ctx.moveTo(points[0][0], points[0][1]);
            for (let i = 1; i < points.length; i++) ctx.lineTo(points[i][0], points[i][1]);
            if (close) ctx.closePath();
        },
        smoothPath(ctx, points, tension, close) {
            tension = tension == null ? 0.5 : tension;
            if (points.length < 2) return;
            const t = tension, n = points.length;
            ctx.beginPath();
            ctx.moveTo(points[0][0], points[0][1]);
            for (let i = 0; i < n - 1; i++) {
                const p0 = points[i - 1] || points[close ? n - 1 : i];
                const p1 = points[i];
                const p2 = points[i + 1];
                const p3 = points[i + 2] || points[close ? (i + 2) % n : i + 1];
                const c1x = p1[0] + (p2[0] - p0[0]) * t / 6;
                const c1y = p1[1] + (p2[1] - p0[1]) * t / 6;
                const c2x = p2[0] - (p3[0] - p1[0]) * t / 6;
                const c2y = p2[1] - (p3[1] - p1[1]) * t / 6;
                ctx.bezierCurveTo(c1x, c1y, c2x, c2y, p2[0], p2[1]);
            }
            if (close) ctx.closePath();
        },
        shadow(ctx, color, blur, dx, dy) {
            ctx.shadowColor = color || 'rgba(0,0,0,0.5)';
            ctx.shadowBlur = blur || 4;
            ctx.shadowOffsetX = dx || 0;
            ctx.shadowOffsetY = dy || 0;
        },
        clearShadow(ctx) {
            ctx.shadowColor = 'transparent';
            ctx.shadowBlur = 0;
            ctx.shadowOffsetX = 0;
            ctx.shadowOffsetY = 0;
        },
    };

    return {
        PICO8, ENDESGA16,
        px, hline, vline, rect, rectOutline,
        line, circle, circleOutline,
        stamp, gradV, mirrorH, hexToRgb,
        smooth,
    };
})();

if (typeof window !== 'undefined') window.brush = brush;
