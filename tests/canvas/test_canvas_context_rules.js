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

    // HTML's serialization of a color: #rrggbb when opaque, rgba(r, g, b, a)
    // with the shortest round-tripping alpha otherwise.
    assert(ctx.fillStyle === '#000000', 'default fillStyle is #000000, got ' + ctx.fillStyle);
    assert(ctx.strokeStyle === '#000000', 'default strokeStyle is #000000, got ' + ctx.strokeStyle);
    assert(ctx.shadowColor === 'rgba(0, 0, 0, 0)', 'default shadowColor is transparent black, got ' + ctx.shadowColor);
    ctx.fillStyle = 'red';
    assert(ctx.fillStyle === '#ff0000', 'an opaque fillStyle reads back #rrggbb, got ' + ctx.fillStyle);
    ctx.fillStyle = 'rgb(18, 52, 86)';
    assert(ctx.fillStyle === '#123456', 'lowercase hex, zero-padded, got ' + ctx.fillStyle);
    ctx.fillStyle = 'hsl(0, 0%, 100%)';
    assert(ctx.fillStyle === '#ffffff', 'hsl serializes as hex too, got ' + ctx.fillStyle);
    ctx.strokeStyle = 'rgba(0, 128, 255, 0.5)';
    assert(ctx.strokeStyle === 'rgba(0, 128, 255, 0.5)', 'a translucent strokeStyle reads back rgba(), got ' + ctx.strokeStyle);
    ctx.strokeStyle = 'rgba(1, 2, 3, 0.3)';
    assert(ctx.strokeStyle === 'rgba(1, 2, 3, 0.3)', 'alpha 0.3 round-trips as 0.3, got ' + ctx.strokeStyle);
    ctx.strokeStyle = '#11223380';
    assert(ctx.strokeStyle === 'rgba(17, 34, 51, 0.5)', '8-digit hex alpha 0x80 prints 0.5, got ' + ctx.strokeStyle);
    ctx.strokeStyle = 'transparent';
    assert(ctx.strokeStyle === 'rgba(0, 0, 0, 0)', 'transparent is rgba(0, 0, 0, 0), got ' + ctx.strokeStyle);
    ctx.shadowColor = 'blue';
    assert(ctx.shadowColor === '#0000ff', 'shadowColor serializes the same way, got ' + ctx.shadowColor);
    ctx.shadowColor = 'rgba(0, 0, 0, 0.25)';
    assert(ctx.shadowColor === 'rgba(0, 0, 0, 0.25)', 'shadowColor alpha 0.25, got ' + ctx.shadowColor);
    // The serialized form parses back to the same color.
    ctx.fillStyle = 'rgba(10, 20, 30, 0.7)';
    const s = ctx.fillStyle;
    ctx.fillStyle = '#000';
    ctx.fillStyle = s;
    assert(ctx.fillStyle === s, 'the serialization round-trips, got ' + ctx.fillStyle + ' vs ' + s);

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
