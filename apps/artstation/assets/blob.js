// "Blob" — a tiny one-eyed creature, 24x24 frames.
// 4-frame idle (squash/stretch breathing), 6-frame walk (bob + foot shuffle).
//
// Color choices come from the ENDESGA16 palette. Body + outline + eye-white
// + pupil are pulled out so they animate consistently across frames.

const P = brush.ENDESGA16;
const BODY     = '#e53b44';   // P[5] — warm red body
const BODY_DK  = '#9e2835';   // P[4] — outline / underside
const EYE      = '#ffffff';   // P[13]
const PUPIL    = '#193d3f';   // P[10]
const FOOT     = '#3f2832';   // P[3]

// Draw the blob given a vertical offset, body height, and foot positions.
// (cx, cy) is the center of the body; helpers handle outline + eye + feet.
function drawBlob(ctx, cx, cy, bodyH, footL, footR, blink) {
    const bodyW = 18;
    const top = cy - bodyH / 2;
    // Body: an ellipse-ish shape using two stacked horizontal slabs.
    // Body interior
    brush.rect(ctx, cx - bodyW/2 + 2, top + 1, bodyW - 4, bodyH - 2, BODY);
    brush.rect(ctx, cx - bodyW/2,     top + 3, bodyW,     bodyH - 6, BODY);
    // Top / bottom rounded caps
    brush.rect(ctx, cx - bodyW/2 + 4, top,         bodyW - 8, 1, BODY);
    brush.rect(ctx, cx - bodyW/2 + 3, top - 0 + 1, bodyW - 6, 0, BODY); // no-op safety
    brush.rect(ctx, cx - bodyW/2 + 4, top + bodyH - 1, bodyW - 8, 1, BODY_DK);
    // Outline (poor-man — just dark pixels along the silhouette top/bottom rows)
    brush.hline(ctx, cx - bodyW/2 + 4, top - 0,   bodyW - 8, BODY_DK);
    brush.hline(ctx, cx - bodyW/2 + 2, top + 1,   2, BODY_DK);
    brush.hline(ctx, cx + bodyW/2 - 4, top + 1,   2, BODY_DK);
    brush.hline(ctx, cx - bodyW/2,     top + 3,   2, BODY_DK);
    brush.hline(ctx, cx + bodyW/2 - 2, top + 3,   2, BODY_DK);
    // Vertical sides
    brush.vline(ctx, cx - bodyW/2,     top + 3,   bodyH - 6, BODY_DK);
    brush.vline(ctx, cx + bodyW/2 - 1, top + 3,   bodyH - 6, BODY_DK);

    // Eye (single big cyclops eye, just below the top of the body).
    const eyeY = top + Math.floor(bodyH * 0.35);
    if (blink) {
        // Closed: a single dark line.
        brush.hline(ctx, cx - 3, eyeY + 1, 6, BODY_DK);
    } else {
        brush.rect(ctx, cx - 3, eyeY,     6, 4, EYE);
        brush.rect(ctx, cx - 1, eyeY + 1, 2, 2, PUPIL);
    }

    // Feet (two small dark stubs at given x offsets relative to center).
    const footY = cy + bodyH/2 - 1;
    brush.rect(ctx, cx + footL - 2, footY, 4, 2, FOOT);
    brush.rect(ctx, cx + footR - 2, footY, 4, 2, FOOT);
}

defineSheet('blob', {
    frameWidth: 24, frameHeight: 24,
    cols: 6, rows: 2,
    bg: 'transparent',
    frames: [
        // ---- Row 0: idle (4 frames + 2 unused) -----------------------
        // Squash/stretch breathing cycle, feet planted, occasional blink.
        (ctx) => drawBlob(ctx, 12, 16, 14, -4, +4, false),
        (ctx) => drawBlob(ctx, 12, 16, 13, -4, +4, false),
        (ctx) => drawBlob(ctx, 12, 16, 14, -4, +4, true ),
        (ctx) => drawBlob(ctx, 12, 16, 15, -4, +4, false),
        null, null,
        // ---- Row 1: walk (6 frames) ---------------------------------
        // Body bobs +/-1 px, feet alternate strides.
        (ctx) => drawBlob(ctx, 12, 16, 14, -5, +3, false),
        (ctx) => drawBlob(ctx, 12, 15, 14, -3, +5, false),
        (ctx) => drawBlob(ctx, 12, 16, 14, -1, +6, false),
        (ctx) => drawBlob(ctx, 12, 16, 14, +3, -5, false),
        (ctx) => drawBlob(ctx, 12, 15, 14, +5, -3, false),
        (ctx) => drawBlob(ctx, 12, 16, 14, +6, -1, false),
    ],
    animations: {
        idle: { frames: [0,1,2,3], fps: 4, loop: true },
        walk: { frames: [6,7,8,9,10,11], fps: 12, loop: true },
    },
});
