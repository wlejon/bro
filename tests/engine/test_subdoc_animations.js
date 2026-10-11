// CSS transitions, keyframe animations and Web Animations in sub-documents:
// an <iframe>'s and a bro.window.open() window's, in the windowed frame loop
// (runFrames) with nothing but their own timers going. A sub-document had no
// animation managers at all, so none of it ran: a transition jumped to its
// end, an animation and element.animate() did nothing, no event fired and no
// finished promise settled. Each now has its own (engine/sub_doc_animations.h),
// on the engine's clock, ticked as it is styled, its events delivered at the
// start of the next frame, and what finished on an idle page settles (as the
// app document's: test_idle_animations.js).
//
// tests/test_app/anim_child, a few frames after it loads: changes a box's colour under a transition,
// starts a keyframe animation (its start and end events), animates a third
// with element.animate() (onfinish, finished), and 400 ms later, the page idle,
// puts in a fading-in tooltip and measures it, as the helmapps kit does.

document.body.style.margin = '0';
document.body.style.background = '#000';

function px(img, x, y) {
    const i = (y * img.width + x) * 4;
    return [img.data[i], img.data[i + 1], img.data[i + 2]];
}
const red = (p) => p && p[0] > 240 && p[1] < 16 && p[2] < 16;
const white = (p) => p && p[0] > 240 && p[1] > 240 && p[2] > 240;
const magenta = (p) => p && p[0] > 240 && p[1] < 16 && p[2] > 240;

function checkResults(where, r) {
    assert(r && r.done, `${where}: the child reported (${JSON.stringify(r)})`);
    if (!r) return;
    const ev = r.events.join(' ');
    assert(r.events.includes('transitionend:background-color'), `${where}: transitionend fired (${ev})`);
    assert(r.events.includes('animationstart:tint') && r.events.includes('animationend:tint'),
           `${where}: animationstart and animationend fired (${ev})`);
    assert(r.onfinish, `${where}: element.animate()'s onfinish fired`);
    assert(r.finished, `${where}: element.animate()'s finished promise settled`);
    assert(r.tipStart !== null && r.tipStart < 20,
           `${where}: the tooltip's fade-in starts as it is put in (currentTime ${r.tipStart})`);
}

// ---- an <iframe> ------------------------------------------------------------
const frame = document.createElement('iframe');
frame.setAttribute('src', 'anim_child');
frame.style.cssText = 'position: absolute; left: 0; top: 0; width: 200px; height: 120px; border: 0';
document.body.appendChild(frame);
flush();
const cw = frame.contentWindow;
assert(cw, 'the iframe child loaded');
runFrames(100, { stepMs: 16 });  // 1.6 s: past every animation and the report
checkResults('iframe', cw.results);
{
    // What the loop presented, settled: the transition's end, the keyframe
    // animation's box back to its own colour, element.animate()'s, the tip.
    runFrames(4, { stepMs: 16 });
    const img = presentedImage();
    const s = devicePixelRatio;
    const at = (x, y) => px(img, Math.round(x * s), Math.round(y * s));
    assert(img, 'iframe: a frame was presented');
    assert(red(at(20, 20)), `iframe: the transition ended on its colour (${at(20, 20)})`);
    assert(red(at(70, 20)), `iframe: the animated box went back to its own colour (${at(70, 20)})`);
    assert(white(at(120, 20)), `iframe: element.animate()'s box is shown whole (${at(120, 20)})`);
    assert(magenta(at(30, 60)), `iframe: the tooltip is on screen (${at(30, 60)})`);
}
frame.remove();
flush();

// ---- a bro.window.open() window ---------------------------------------------
const win = bro.window.open('anim_child', { width: 200, height: 120 });
let loaded = 0, report = null;
win.addEventListener('load', () => { loaded++; });
win.addEventListener('message', (e) => { report = e.data; });
flush();
assert(loaded === 1, 'the window child loaded');
runFrames(100, { stepMs: 16 });
flush();  // the report's message
checkResults('window', report);
{
    const shot = win.capture();
    assert(shot, 'window: captured');
    if (shot) {
        const s = shot.width / 200;
        const at = (x, y) => px(shot, Math.round(x * s), Math.round(y * s));
        assert(red(at(20, 20)), `window: the transition ended on its colour (${at(20, 20)})`);
        assert(red(at(70, 20)), `window: the animated box went back to its own colour (${at(70, 20)})`);
        assert(magenta(at(30, 60)), `window: the tooltip is shown (${at(30, 60)})`);
    }
}
win.close();
flush();
console.log('sub-document animations: an iframe and a window ran them');
