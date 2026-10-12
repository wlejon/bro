// Driven by tests/engine/test_panel_animations.js in a child bro-headless on
// tests/test_app/panel_anim_app: F8 shows its perf panel (the app's own
// system/perf.html), which animates once shown and logs PANEL_ANIM; here, what
// the frame loop presented once they all ended.

function px(img, x, y) {
    const i = (y * img.width + x) * 4;
    return [img.data[i], img.data[i + 1], img.data[i + 2]];
}
const red = (p) => p && p[0] > 240 && p[1] < 16 && p[2] < 16;
const white = (p) => p && p[0] > 240 && p[1] > 240 && p[2] > 240;
const magenta = (p) => p && p[0] > 240 && p[1] < 16 && p[2] > 240;

keyDown('F8');
keyUp('F8');
runFrames(100, { stepMs: 16 });  // 1.6 s: past every animation and the report
runFrames(4, { stepMs: 16 });
const img = presentedImage();
const s = devicePixelRatio;
const at = (x, y) => px(img, Math.round(x * s), Math.round(y * s));
assert(img, 'a frame was presented');
assert(red(at(20, 20)), `the transition ended on its colour (${at(20, 20)})`);
assert(red(at(70, 20)), `the animated box went back to its own colour (${at(70, 20)})`);
assert(white(at(120, 20)), `element.animate()'s box is shown whole (${at(120, 20)})`);
assert(magenta(at(20, 60)), `the tooltip faded in (${at(20, 60)})`);
console.log('PANEL_ANIM_PIXELS ok');
