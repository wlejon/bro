// Trivial smoke test — 4-frame "blink" of a colored square.
defineSheet('smoke', {
    frameWidth: 16, frameHeight: 16,
    cols: 4, rows: 1,
    frames: [
        (ctx) => { brush.rect(ctx, 2, 2, 12, 12, '#f00'); },
        (ctx) => { brush.rect(ctx, 2, 2, 12, 12, '#ff0'); },
        (ctx) => { brush.rect(ctx, 2, 2, 12, 12, '#0f0'); },
        (ctx) => { brush.rect(ctx, 2, 2, 12, 12, '#0ff'); },
    ],
    animations: {
        blink: { frames: [0,1,2,3], fps: 4, loop: true },
    },
});
