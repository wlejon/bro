// CSS transitions, keyframe animations and Web Animations in a system panel,
// as in an <iframe> (test_subdoc_animations.js). A panel had no animation
// managers: a transition jumped to its end, an animation and element.animate()
// did nothing, no event fired and no finished promise settled. Each panel now
// has its own (engine/sub_doc_animations.h), on the engine's clock, ticked as
// it is laid out, its events delivered at the start of the next frame.
//
// The panel is an app's own: tests/test_app/panel_anim_app's system/perf.html
// stands for the perf panel, so this runs tests/engine/panel_animations.js in
// a child bro-headless on that app. It shows the panel (F8), which changes a
// box's colour under a transition, starts a keyframe animation, animates a
// third box with element.animate(), and 400 ms later puts in a fading-in
// tooltip; the panel logs what happened (PANEL_ANIM), the driver what was
// presented (PANEL_ANIM_PIXELS).

const cp = require('child_process');
const path = require('path');

const exeName = process.platform === 'win32' ? 'bro-headless.exe' : 'bro-headless';
const exe = path.join(process.env.BRO_EXE_DIR, exeName);
const app = path.join(process.env.BRO_APP_DIR, 'panel_anim_app');
const driver = path.join(process.env.BRO_APP_DIR, '..', 'engine', 'panel_animations.js');
const r = cp.spawnSync(exe, [app, driver], { encoding: 'utf8', env: process.env });
const out = (r.stdout || '') + (r.stderr || '');
const lines = out.split(/\r?\n/);
assert(r.status === 0, `the panel run passed (exit ${r.status})\n` +
       lines.filter((l) => /ASSERT|PANEL_ANIM|ERROR/.test(l)).join('\n'));

const line = lines.find((l) => l.includes('PANEL_ANIM {'));
assert(line, `the panel reported:\n${lines.filter((l) => /PANEL|ERROR|ASSERT/.test(l)).join('\n')}`);
if (line) {
    const res = JSON.parse(line.slice(line.indexOf('{')));
    const ev = res.events.join(' ');
    assert(res.events.includes('transitionend:background-color'), `transitionend fired (${ev})`);
    assert(res.events.includes('animationstart:tint') && res.events.includes('animationend:tint'),
           `animationstart and animationend fired (${ev})`);
    assert(res.onfinish, "element.animate()'s onfinish fired");
    assert(res.finished, "element.animate()'s finished promise settled");
    assert(res.tipStart !== null && res.tipStart < 20,
           `the tooltip's fade-in starts as it is put in (currentTime ${res.tipStart})`);
    assert(res.tipMid > 0 && res.tipMid < 1, `the tooltip is part way in 50 ms later (opacity ${res.tipMid})`);
}
assert(lines.some((l) => l.includes('PANEL_ANIM_PIXELS ok')), 'the presented frame shows where they ended');
console.log('panel animations: transitions, animations and Web Animations ran in a system panel');
