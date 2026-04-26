// Validates smooth-mode and color utils.
const base = '#3b82f6';
const accent = color.complementary(base)[1];
const ramp = color.ramp(base, 0.15);

defineSheet('smooth_test', {
    frameWidth: 96, frameHeight: 96,
    cols: 2, rows: 1,
    pixel: false,             // turn ON antialiasing + curves
    bg: 'transparent',
    frames: [
        // Smooth circle with radial gradient + drop shadow
        (ctx, w, h) => {
            brush.smooth.shadow(ctx, 'rgba(0,0,0,0.5)', 8, 0, 4);
            const grad = brush.smooth.radialGradient(ctx, w*0.4, h*0.4, 4, w*0.5, [
                [0, ramp[4]], [0.6, base], [1, ramp[0]]
            ]);
            brush.smooth.circle(ctx, w/2, h/2, w*0.4, grad);
            brush.smooth.clearShadow(ctx);
            // crescent highlight
            ctx.globalCompositeOperation = 'source-atop';
            brush.smooth.ellipse(ctx, w*0.4, h*0.35, w*0.18, h*0.08, -0.4,
                                 'rgba(255,255,255,0.45)');
            ctx.globalCompositeOperation = 'source-over';
        },

        // Smooth-path leaf shape
        (ctx, w, h) => {
            const pts = [
                [w*0.5, h*0.1], [w*0.85, h*0.4], [w*0.7, h*0.85],
                [w*0.5, h*0.95], [w*0.3, h*0.85], [w*0.15, h*0.4],
            ];
            brush.smooth.smoothPath(ctx, pts, 0.6, true);
            ctx.fillStyle = brush.smooth.linearGradient(ctx, 0, 0, w, h, [
                [0, color.lighten(accent, 0.15)],
                [1, color.darken(accent, 0.2)],
            ]);
            ctx.fill();
            ctx.lineWidth = 2;
            ctx.strokeStyle = color.darken(accent, 0.3);
            ctx.stroke();
            // central vein
            ctx.beginPath();
            ctx.moveTo(w*0.5, h*0.15);
            ctx.bezierCurveTo(w*0.5, h*0.4, w*0.5, h*0.7, w*0.5, h*0.92);
            ctx.lineWidth = 1.5;
            ctx.strokeStyle = color.darken(accent, 0.4);
            ctx.stroke();
        },
    ],
});
