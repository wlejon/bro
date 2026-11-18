// getComputedStyle(el).transform is the matrix the transform amounts to, as
// Chromium resolves it: matrix(a, b, c, d, e, f) for a 2D one, matrix3d(...)
// otherwise, percentages against the border box, numbers in %g form (rounding
// noise such as Chromium's 6.12323e-17 for cos(90deg) prints as 0); "none"
// stays "none". (A Web Animations page read translateX(100px) mid-animation.)

document.body.innerHTML = '<div id="a"></div><div id="b"></div><div id="c"></div><div id="d"></div><div id="e"></div>';
const set = (id, t, extra) => { const el = document.getElementById(id); el.style.cssText = 'box-sizing:content-box;width:100px;height:50px;' + (extra || '') + 'transform:' + t; return el; };
const a = set('a', 'translateX(10px) scale(2)');
const b = set('b', 'rotate(90deg)');
const c = set('c', 'translate(50%, 10%)', 'padding:10px;');
const d = set('d', 'translateZ(5px)');
const e = document.getElementById('e');
flush();
const t = (el) => getComputedStyle(el).transform;
assert(t(a) === 'matrix(2, 0, 0, 2, 10, 0)', 'translate then scale, got ' + t(a));
assert(t(b) === 'matrix(0, 1, -1, 0, 0, 0)', 'rotate(90deg), got ' + t(b));
assert(t(c) === 'matrix(1, 0, 0, 1, 60, 7)', 'percentages against the border box (120 x 70), got ' + t(c));
assert(t(d) === 'matrix3d(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 5, 1)', 'a 3D one is matrix3d, got ' + t(d));
assert(t(e) === 'none', 'no transform is none, got ' + t(e));
assert(e.style.transform === '' && a.style.transform === 'translateX(10px) scale(2)', 'the inline style keeps what was written');

const anim = e.animate([{ transform: 'translateX(0px)' }, { transform: 'translateX(200px)' }], { duration: 1000 });
anim.pause();
anim.currentTime = 500;
assert(t(e) === 'matrix(1, 0, 0, 1, 100, 0)', 'mid-animation, got ' + t(e));

console.log('test_computed_transform_matrix: OK');
