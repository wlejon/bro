// Canvas context rules restored from the pre-bronze bindings:
//   - one context TYPE per canvas: after getContext('2d'), getContext('webgl2')
//     answers null (and vice versa), as the HTML spec says;
//   - a WebGL canvas's width/height setters resize the drawing buffer and a
//     zero-size assignment is refused rather than allocating an empty FBO;
//   - fillStyle / strokeStyle read back as "rgba(r,g,b,a.aa)" with a two-decimal
//     alpha, the format the old getter used and canvas libraries parse;
//   - an unknown lineCap / lineJoin string is ignored (the old value stays);
//   - getLineDash returns the list as set — an odd-length list is not doubled
//     at set time (the painter doubles it, as the spec says).

// --- context type lock ------------------------------------------------------
{
    const c = document.createElement('canvas');
    c.width = 64; c.height = 64;
    document.body.appendChild(c);
    flush();
    const ctx = c.getContext('2d');
    assert(ctx !== null, '2d context on a fresh canvas');
    assert(c.getContext('2d') === ctx, 'getContext("2d") again returns the same context');
    assert(c.getContext('webgl2') === null, 'webgl2 after 2d is refused');
    assert(c.getContext('webgl') === null, 'webgl after 2d is refused');
    assert(c.getContext('scene') === null, 'scene after 2d is refused');
    assert(ctx instanceof CanvasRenderingContext2D, 'ctx instanceof CanvasRenderingContext2D');
    assert(typeof CanvasRenderingContext2D === 'function', 'CanvasRenderingContext2D is a global');
    let threw = false;
    try { new CanvasRenderingContext2D(); } catch (e) { threw = true; }
    assert(threw, 'CanvasRenderingContext2D is not constructible');
}
{
    const c = document.createElement('canvas');
    c.width = 64; c.height = 64;
    document.body.appendChild(c);
    flush();
    const gl = c.getContext('webgl2');
    if (gl) {
        assert(c.getContext('2d') === null, '2d after webgl2 is refused');
        assert(c.getContext('webgl2') === gl, 'getContext("webgl2") again returns the same context');

        // --- WebGL canvas sizing ---
        c.width = 128;
        c.height = 96;
        assert(gl.drawingBufferWidth === 128, 'width setter resized the drawing buffer, got ' + gl.drawingBufferWidth);
        assert(gl.drawingBufferHeight === 96, 'height setter resized the drawing buffer, got ' + gl.drawingBufferHeight);
        assert(c.width === 128 && c.height === 96, 'canvas width/height read back the buffer size');
        c.width = 0;
        assert(gl.drawingBufferWidth === 128, 'zero width leaves the drawing buffer alone');
        c.setAttribute('height', '48');
        assert(gl.drawingBufferHeight === 48, 'setAttribute("height") resizes the drawing buffer too, got ' + gl.drawingBufferHeight);
    } else {
        console.log('webgl2 unavailable; skipping the WebGL sizing checks');
    }
}

// --- fillStyle format, lineCap / lineJoin, getLineDash ---------------------
{
    const c = document.createElement('canvas');
    c.width = 32; c.height = 32;
    document.body.appendChild(c);
    flush();
    const ctx = c.getContext('2d');

    ctx.fillStyle = 'red';
    assert(ctx.fillStyle === 'rgba(255,0,0,1.00)', 'fillStyle reads back rgba(r,g,b,a.aa), got ' + ctx.fillStyle);
    ctx.strokeStyle = 'rgba(0, 128, 255, 0.5)';
    assert(ctx.strokeStyle === 'rgba(0,128,255,0.50)', 'strokeStyle keeps a two-decimal alpha, got ' + ctx.strokeStyle);

    ctx.lineCap = 'round';
    ctx.lineCap = 'bogus';
    assert(ctx.lineCap === 'round', 'unknown lineCap ignored, got ' + ctx.lineCap);
    ctx.lineCap = 'square';
    assert(ctx.lineCap === 'square', 'valid lineCap accepted');
    ctx.lineJoin = 'bevel';
    ctx.lineJoin = 'nope';
    assert(ctx.lineJoin === 'bevel', 'unknown lineJoin ignored, got ' + ctx.lineJoin);
    ctx.lineJoin = 'miter';
    assert(ctx.lineJoin === 'miter', 'valid lineJoin accepted');

    ctx.setLineDash([4, 2, 1]);
    const dash = ctx.getLineDash();
    assert(Array.isArray(dash) && dash.length === 3 && dash[0] === 4 && dash[1] === 2 && dash[2] === 1,
           'odd-length dash list reads back as set, got ' + JSON.stringify(dash));
    ctx.setLineDash([]);
    assert(ctx.getLineDash().length === 0, 'empty dash list clears');
    // Stroking with an odd list must still paint (the painter doubles it).
    ctx.setLineDash([3]);
    ctx.strokeStyle = 'black';
    ctx.beginPath(); ctx.moveTo(0, 16); ctx.lineTo(32, 16); ctx.stroke();
    flush();
}

console.log('canvas context rules OK');
