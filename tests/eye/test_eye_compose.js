function assert(cond, msg) {
    if (!cond) throw new Error('ASSERT FAILED: ' + msg);
}

document.body.style.cssText = 'margin:0;width:640px;height:400px;background:#101216;overflow:hidden;position:relative;font-family:sans-serif;';

const makeBox = (id, x, y, w, h, bg, text) => {
    const el = document.createElement('div');
    el.id = id;
    el.style.cssText = `position:absolute;left:${x}px;top:${y}px;width:${w}px;height:${h}px;background:${bg};color:#eef1f6;font-size:14px;padding:8px;box-sizing:border-box;`;
    if (text) el.textContent = text;
    document.body.appendChild(el);
    return el;
};

// Hero focal mass + aligned column at x=24 + a +3px near-miss drift at x=27
const hero = makeBox('hero', 24, 20, 260, 120, '#3a82f6', 'Primary Hero Focus');
hero.style.fontWeight = '700';
const cardA = makeBox('card-a', 24, 160, 260, 44, '#1b1f27', 'Aligned item one');
const cardB = makeBox('card-b', 27, 220, 260, 44, '#1b1f27', 'Drifted item two');

// Tangent kiss pair (2px separation between sibling containers)
makeBox('kiss-a', 320, 20, 120, 60, '#222731', 'Left box');
makeBox('kiss-b', 442, 20, 120, 60, '#222731', 'Right box');

// Squeezed narrow multiline text block
makeBox('narrow-wrap', 320, 110, 88, 96, '#1b1f27', 'Long label text that wraps awkwardly down');

flush();

const comp = bro.eye.compose(null, { layout: true, maxAnchors: 4, notanTones: 3 });
assert(comp.width > 0 && comp.height > 0, 'compose captures frame dimensions');
assert(comp.saliency.anchors.length >= 1, 'saliency extracts focal anchors');
assert(comp.saliency.anchors[0].x < 0.55 && comp.saliency.anchors[0].y < 0.55,
    'primary focal anchor lands near hero card, got ' + JSON.stringify(comp.saliency.anchors[0]));
assert(comp.notan.massCount >= 2 && comp.notan.darkShare > 0.3, 'notan captures mass structure');
assert(comp.rhythm.restRatio > 0.2 && comp.rhythm.restDetailContrast > 0.0, 'rhythm computes rest vs detail');

assert(comp.layout.analyzed === true, 'layout analysis enabled');
assert(comp.layout.drifts.some(d => d.selector.includes('card-b') && Math.abs(d.driftPx - 3.0) < 0.5),
    'detects +3px near-miss rail drift on #card-b, got: ' + JSON.stringify(comp.layout.drifts));
assert(comp.layout.wrapIssues.some(w => w.selector.includes('narrow-wrap')),
    'detects awkward narrow wrap on #narrow-wrap, got: ' + JSON.stringify(comp.layout.wrapIssues));
assert(comp.layout.tangents.some(t => Math.abs(t.gapPx - 2.0) < 0.5),
    'detects 2px tangent kiss pair, got: ' + JSON.stringify(comp.layout.tangents));

// Compare perceptual deltas after moving #hero right
const refFrame = bro.eye.visualize(null, { mode: 'sheet', maxPanelDim: 640 });
hero.style.left = '340px';
flush();
const cmp = bro.eye.compare(null, { width: refFrame.width, height: refFrame.height, data: refFrame.data });
assert(cmp.perceptual && cmp.perceptual.focalShiftDist > 0.02,
    'perceptual diff captures focal shift when hero moves, got ' + JSON.stringify(cmp.perceptual));

// Render new taste visualizers
const vSal = bro.eye.visualize(null, { mode: 'saliency', maxPanelDim: 480 });
assert(vSal.panels.length === 2 && vSal.width > 100, 'saliency visualization builds 2 panels');
const vNot = bro.eye.visualize(null, { mode: 'notan', notanTones: 5, maxPanelDim: 480 });
assert(vNot.panels.length === 2 && vNot.width > 100, 'notan visualization builds 2 panels');
const vRail = bro.eye.visualize(null, { mode: 'rails', maxPanelDim: 480 });
assert(vRail.panels.length === 1 && vRail.width > 100, 'rails visualization builds 1 annotated panel');

console.log('test_eye_compose PASS');
