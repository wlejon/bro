// Minimal atlas test: 3 plain rectangles of different sizes.
defineAtlas('icons', {
    pixel: true,
    padding: 2,
    maxWidth: 64,
    bg: 'transparent',
    regions: [
        { name: 'a', width: 16, height: 16, draw(ctx) {
            ctx.fillStyle = '#ff0000';
            ctx.fillRect(0, 0, 16, 16);
        }},
        { name: 'b', width: 24, height: 24, draw(ctx) {
            ctx.fillStyle = '#00ff00';
            ctx.fillRect(0, 0, 24, 24);
        }},
        { name: 'c', width: 32, height: 16, draw(ctx) {
            ctx.fillStyle = '#0000ff';
            ctx.fillRect(0, 0, 32, 16);
        }},
    ],
});
