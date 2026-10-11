// A 2D canvas taken out of the document and put back keeps its bitmap and
// its context, as in a browser: removing an element never destroys it.
//
// Regression: the frame's sweep took a canvas found out of the document
// (after it had been in it) for a dead one. It dropped the element's
// CanvasScene, so the canvas painted nothing once it was back, and its
// context, still the one getContext('2d') answers, drew into nothing. A
// virtual list recycles its cells exactly so (a cell scrolled out is removed
// and appended again for another row a frame later): every thumbnail on a
// recycled cell went blank (helmapps' Image Viewer, the stabilize view's
// frame row after about 18 frames). Checked through the headless step and
// through the windowed frame loop, which sweeps on its own path.

const root = document.createElement('div');
root.style.cssText = 'position:absolute; left:0; top:0;';
document.body.style.margin = '0';
document.body.appendChild(root);

function makeCanvas(x, color) {
    const c = document.createElement('canvas');
    c.width = 40; c.height = 40;
    c.style.cssText = `position:absolute; left:${x}px; top:0; width:40px; height:40px;`;
    root.appendChild(c);
    const ctx = c.getContext('2d');
    ctx.fillStyle = color;
    ctx.fillRect(0, 0, 40, 40);
    return { c, ctx };
}

const near = (p, rgb) => Math.abs(p.r - rgb[0]) < 8 && Math.abs(p.g - rgb[1]) < 8 && Math.abs(p.b - rgb[2]) < 8;
const RED = [255, 0, 0], BLUE = [0, 0, 255], GREEN = [0, 128, 0];

function check(where, x, rgb, read) {
    const p = read(x + 20, 20);
    assert(near(p, rgb), `${where}: expected rgb(${rgb}) at x=${x + 20}, got rgb(${p.r}, ${p.g}, ${p.b})`);
}

// Pixels from the headless step.
const stepRead = (x, y) => getPixel(x, y);
// Pixels from the frame the windowed loop last presented.
function windowRead(x, y) {
    const img = presentedImage();
    assert(img, 'the frame loop presented a frame');
    const s = img.width / window.innerWidth;
    const i = (Math.floor(y * s) * img.width + Math.floor(x * s)) * 4;
    return { r: img.data[i], g: img.data[i + 1], b: img.data[i + 2], a: img.data[i + 3] };
}

function run(name, frames, read, x) {
    const a = makeCanvas(x, 'red');
    frames();
    check(`${name}: drawn`, x, RED, read);

    // Out of the document for frames (the sweep runs), then back.
    a.c.remove();
    frames();
    frames();
    root.appendChild(a.c);
    frames();
    check(`${name}: back in the document, its bitmap kept`, x, RED, read);

    // Its context still draws.
    a.ctx.fillStyle = 'blue';
    a.ctx.fillRect(0, 0, 40, 40);
    frames();
    check(`${name}: drawn after coming back`, x, BLUE, read);
    assert(a.c.getContext('2d') === a.ctx, `${name}: getContext answers the same context`);

    // Drawn while out of the document: shown once it is back.
    a.c.remove();
    frames();
    a.ctx.fillStyle = 'green';
    a.ctx.fillRect(0, 0, 40, 40);
    frames();
    root.appendChild(a.c);
    frames();
    check(`${name}: drawn while out, shown when back`, x, GREEN, read);
    const d = a.ctx.getImageData(5, 5, 1, 1).data;
    assert(d[1] > 100 && d[0] < 20, `${name}: getImageData reads the green (${d[0]}, ${d[1]}, ${d[2]})`);
}

run('step', () => { flush(); advanceTime(16); flush(); }, stepRead, 0);
run('window', () => { runFrames(6, { stepMs: 16 }); }, windowRead, 60);

// A canvas removed for good still goes: churn without growth in the scene count is
// test_canvas_detach_churn's; here, that a removed and unreferenced canvas does not
// keep the frame from drawing the ones left.
for (let i = 0; i < 20; i++) makeCanvas(120 + (i % 4) * 2, 'red').c.remove();
flush(); advanceTime(16); flush();
check('after churn: the first canvas', 0, GREEN, stepRead);

console.log('test_canvas_reattach PASSED');
