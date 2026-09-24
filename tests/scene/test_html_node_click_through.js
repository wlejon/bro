// A world-anchored HtmlNode takes the pointer only where it shows content.
// Its transparent rest (the bare <html>/<body> backdrop), and content marked
// pointer-events: none, pass the click to the canvas behind. The billboard
// used to swallow every press over its whole layout rect.

const canvas = document.createElement('canvas');
canvas.style.width = '400px';
canvas.style.height = '300px';
canvas.setAttribute('width', '400');
canvas.setAttribute('height', '300');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
if (!scene) {
    console.log('no scene; skipping html node click-through test');
} else {
    scene.setCamera({ fov: 60, near: 0.1, far: 100, position: [0, 0, 5], target: [0, 0, 0], up: [0, 1, 0] });
    let downs = 0;
    canvas.addEventListener('mousedown', () => { downs++; });
    const settle = () => { advanceTime(16); flush(); advanceTime(16); flush(); };

    // A 200x100 px tag at 100 px per unit: a 2 x 1 world quad centred on the
    // origin. A 40x20 red box sits in its top-left corner; the rest is bare.
    const tag = scene.createHtmlNode({
        html: '<div id="box" style="width:40px;height:20px;background:#f00"></div>',
        width: 200, height: 100, pxPerUnit: 100, worldAnchor: [0, 0, 0],
    });
    settle();

    // Page pixel of a world point.
    const at = (x, y, z) => {
        const r = canvas.getBoundingClientRect();
        const p = scene.projectLocal(x, y, z);
        return [r.left + p.x, r.top + p.y];
    };
    const press = (p) => { const before = downs; click(p[0], p[1]); flush(); return downs - before; };
    const onBox = at(-0.8, 0.4, 0);        // inside the red box
    const bare = at(0.3, -0.2, 0);          // tag backdrop, nothing drawn
    const outside = at(1.6, 0, 0);          // off the tag entirely

    assert(press(outside) === 1, 'a click beside the tag reaches the canvas');
    assert(press(bare) === 1, 'a click on the transparent part of the tag reaches the canvas');
    assert(press(onBox) === 0, 'a click on the tag content goes to the tag, not the canvas');

    // pointer-events: none content lets the click through too.
    tag.setHtml('<div style="width:40px;height:20px;background:#f00;pointer-events:none"></div>');
    settle();
    assert(press(onBox) === 1, 'pointer-events: none content passes the click through');

    // A panel that fills the tag is content everywhere, transparent or not.
    tag.setHtml('<div style="width:200px;height:100px"></div>');
    settle();
    assert(press(bare) === 0, 'a full-size panel takes clicks over its whole rect');
    tag.setHtml('<div style="width:200px;height:100px;pointer-events:none"><b style="pointer-events:auto">hi</b></div>');
    settle();
    assert(press(bare) === 1, 'a pointer-events: none panel passes clicks between its live children');

    // A second, content-free tag in front of the first does not hide it.
    tag.setHtml('<div style="width:40px;height:20px;background:#f00"></div>');
    const front = scene.createHtmlNode({ html: '', width: 200, height: 100, pxPerUnit: 100, worldAnchor: [0, 0, 1] });
    settle();
    const onBoxThroughFront = at(-0.8, 0.4, 0);
    assert(press(onBoxThroughFront) === 0, 'a bare tag in front passes the click to the tag behind');
    assert(press(at(0.3, -0.2, 0)) === 1, 'and past both bare backdrops to the canvas');

    front.destroy();
    tag.destroy();
    console.log('html node click-through: ok');
}
