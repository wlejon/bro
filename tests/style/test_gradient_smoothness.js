// CSS gradients paint smooth ramps on the GPU path (Ganesh-Vulkan), the way
// they do on the CPU. A gradient with three or more intervals (a stop not at
// 0%/100%, or more than three stops) runs Skia's binary-search colorizer,
// which reads `scale[pos]` / `bias[pos]` with a per-pixel index. While Skia
// kept those arrays in push constants, RADV read them wrong for waves whose
// pixels straddled a stop: blocks of the box took a neighbouring interval's
// ramp and extrapolated past its stop colours, painting hue-shifted squares
// every few dozen px. The check is per row over the whole box: no pixel may
// jump away from the line through its two neighbours, and a flat region
// (before the first stop, after the last) stays exactly the stop colour.

document.body.style.cssText = 'margin:0;background:rgb(32,32,48)';

function block(id) {
    const r = document.getElementById(id).getBoundingClientRect();
    return { r, img: getPixels(Math.round(r.left), Math.round(r.top),
                               Math.round(r.width), Math.round(r.height)) };
}

// Largest |p[x] - (p[x-1] + p[x+1]) / 2| over every row and channel: a smooth
// ramp's second difference is ~0, a block that took another ramp's colour
// stands out by tens of levels.
function worstKink(img, x0 = 1, x1 = img.width - 1) {
    let worst = 0, at = '';
    for (let y = 0; y < img.height; ++y) {
        for (let x = Math.max(1, x0); x < Math.min(img.width - 1, x1); ++x) {
            const i = (y * img.width + x) * 4;
            for (let c = 0; c < 3; ++c) {
                const d = Math.abs(img.data[i + c] - (img.data[i - 4 + c] + img.data[i + 4 + c]) / 2);
                if (d > worst) { worst = d; at = `x=${x} y=${y} c=${c}`; }
            }
        }
    }
    return { worst, at };
}

function show(html) {
    document.body.innerHTML = html;
    for (let i = 0; i < 2; ++i) { advanceTime(16); flush(); }
}

// --- linear, interior stops: flat 0..24%, ramp, flat 76..100% -------------
show('<div id="g" style="position:absolute;left:20px;top:20px;width:600px;height:24px;' +
     'background:linear-gradient(90deg, rgb(90 140 255) 24%, rgb(190 100 240) 76%)"></div>');
{
    const { img } = block('g');
    const k = worstKink(img);
    assert(k.worst <= 3, `linear-gradient with interior stops is smooth (worst kink ${k.worst} at ${k.at})`);
    let off = 0;
    for (let y = 0; y < img.height; ++y) {
        for (let x = 0; x < img.width; ++x) {
            const i = (y * img.width + x) * 4;
            const flatL = x < img.width * 0.24 - 2, flatR = x > img.width * 0.76 + 2;
            if (!flatL && !flatR) continue;
            const want = flatL ? [90, 140, 255] : [190, 100, 240];
            for (let c = 0; c < 3; ++c) if (Math.abs(img.data[i + c] - want[c]) > 2) off++;
        }
    }
    assert(off === 0, `the clamped ends stay the stop colour, ${off} channel(s) extrapolated past it`);
}

// --- linear, five stops fading in and out over the page colour -----------
show('<div id="g" style="position:absolute;left:20px;top:20px;width:600px;height:24px;' +
     'background:linear-gradient(90deg, rgb(90 140 255 / 0), rgb(90 140 255) 24%, ' +
     'rgb(190 100 240) 50%, rgb(255 180 60) 76%, rgb(255 180 60 / 0))"></div>');
{
    const { img } = block('g');
    // The stops themselves are kinks by construction; skip a few px around each.
    let worst = 0, at = '';
    for (const [a, b] of [[0, 0.24], [0.24, 0.5], [0.5, 0.76], [0.76, 1]]) {
        const k = worstKink(img, Math.ceil(a * img.width) + 3, Math.floor(b * img.width) - 3);
        if (k.worst > worst) { worst = k.worst; at = k.at; }
    }
    assert(worst <= 3, `multi-stop linear-gradient is smooth between stops (worst kink ${worst} at ${at})`);
}

// --- radial, interior stops ------------------------------------------------
show('<div id="g" style="position:absolute;left:20px;top:20px;width:400px;height:400px;' +
     'background:radial-gradient(circle 200px at center, rgb(255 255 255) 10%, rgb(255 0 0) 40%, ' +
     'rgb(0 0 255) 70%, rgb(0 0 0) 90%)"></div>');
{
    const { img } = block('g');
    // Rows only see the ramp curve gently away from the centre; compare each
    // pixel with its neighbours, skipping the rings at the stop radii.
    let worst = 0, at = '';
    const stopsPx = [20, 80, 140, 180];
    for (let y = 0; y < img.height; y += 3) {
        for (let x = 1; x < img.width - 1; ++x) {
            const d = Math.hypot(x + 0.5 - 200, y + 0.5 - 200);
            if (d < 8 || stopsPx.some(s => Math.abs(d - s) < 3)) continue;
            const i = (y * img.width + x) * 4;
            for (let c = 0; c < 3; ++c) {
                const k = Math.abs(img.data[i + c] - (img.data[i - 4 + c] + img.data[i + 4 + c]) / 2);
                if (k > worst) { worst = k; at = `x=${x} y=${y} c=${c}`; }
            }
        }
    }
    assert(worst <= 4, `radial-gradient with interior stops is smooth (worst kink ${worst} at ${at})`);
}

// --- conic, several stops ---------------------------------------------------
show('<div id="g" style="position:absolute;left:20px;top:20px;width:400px;height:400px;' +
     'background:conic-gradient(from 210deg, rgb(255 0 0), rgb(255 255 0) 20%, rgb(0 255 0) 40%, ' +
     'rgb(0 0 255) 70%, rgb(255 0 0))"></div>');
{
    const { img } = block('g');
    // Along a row far from the centre the angle changes slowly; a stop's
    // angle is still a kink, so judge by the second difference with a
    // margin a stop's corner fits under but a wrong-interval block does not.
    let worst = 0, at = '';
    for (const y of [0, 10, 20, 30, 370, 380, 390, 399]) {
        for (let x = 1; x < img.width - 1; ++x) {
            const i = (y * img.width + x) * 4;
            for (let c = 0; c < 3; ++c) {
                const k = Math.abs(img.data[i + c] - (img.data[i - 4 + c] + img.data[i + 4 + c]) / 2);
                if (k > worst) { worst = k; at = `x=${x} y=${y} c=${c}`; }
            }
        }
    }
    assert(worst <= 6, `conic-gradient with several stops is smooth (worst kink ${worst} at ${at})`);
}

console.log('gradient smoothness OK');
