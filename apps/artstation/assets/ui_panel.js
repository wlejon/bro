// Smooth UI panel — dark glassy background with a soft border ring and
// an inner highlight. Nine-slice rects let game code stretch the middle
// and tile the edges to any size without distorting the corners.

const PANEL_BG       = '#1f2230';
const PANEL_BG_LIGHT = '#2a2f44';
const PANEL_BORDER   = '#5c6680';
const PANEL_HILITE   = '#9aa6c2';

const SIZE   = 64;          // 64x64 base, slice 16 around → 32x32 stretchy core
const SLICE  = 16;
const RADIUS = 12;

defineNineSlice('ui_panel', {
    width: SIZE, height: SIZE,
    pixel: false,
    slice: { left: SLICE, right: SLICE, top: SLICE, bottom: SLICE },
    bg: 'transparent',
    draw(ctx, w, h) {
        // Background gradient
        ctx.fillStyle = brush.smooth.linearGradient(ctx, 0, 0, 0, h, [
            [0, PANEL_BG_LIGHT],
            [1, PANEL_BG],
        ]);
        brush.smooth.roundRect(ctx, 1, 1, w - 2, h - 2, RADIUS, ctx.fillStyle);

        // Outer border
        brush.smooth.roundRectOutline(ctx, 1.5, 1.5, w - 3, h - 3,
                                       RADIUS, PANEL_BORDER, 1.5);

        // Inner highlight (a thin lighter line just inside)
        ctx.globalAlpha = 0.35;
        brush.smooth.roundRectOutline(ctx, 3.5, 3.5, w - 7, h - 7,
                                       RADIUS - 2, PANEL_HILITE, 1);
        ctx.globalAlpha = 1.0;

        // Subtle inner shadow at the top edge for "set in" feel
        const shadowGrad = brush.smooth.linearGradient(ctx, 0, 0, 0, 8, [
            [0, 'rgba(0,0,0,0.35)'],
            [1, 'rgba(0,0,0,0)'],
        ]);
        ctx.save();
        brush.smooth.roundRect(ctx, 2, 2, w - 4, h - 4, RADIUS - 1, shadowGrad);
        ctx.restore();
    },
});
