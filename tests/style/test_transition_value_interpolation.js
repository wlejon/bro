// Values that are more than one number interpolate every part of themselves.
//
// transform lists of different shapes (CSS Transforms 2): the pairs that
// share a primitive blend argument by argument, and from the first pair that
// does not, the rest blends through matrix decomposition — a rotation turns,
// a scale scales, instead of the whole list flipping at 50%.
//
// Multi-value strings: every number and colour in `box-shadow`,
// `text-shadow`, a `filter` function, a two-value length blends, and a
// shadow list shorter than the other is padded with transparent shadows.
//
// Before: a transform pair of different shapes flipped halfway, and the
// number interpolator read the first number of a multi-value string and
// copied the rest (`0px 0px 0px black` → `10px 20px 30px red` ran as
// `5 20px 30px red`).

const root = document.getElementById('root');
const props = [
    ['m2d', 'transform', 'rotate(0)', 'translateX(10px) scale(2)'],
    ['rot', 'transform', 'scale(1)', 'rotate(90deg) translateX(10px)'],
    ['m3d', 'transform', 'translateZ(0px)', 'rotateX(90deg) scale(2)'],
    ['axis', 'transform', 'rotateY(0deg)', 'rotateX(90deg)'],
    ['prefix', 'transform', 'translateX(0px) rotate(0deg)', 'translateX(100px) scale(3)'],
    ['same', 'transform', 'translateX(0px)', 'translateX(100px)'],
    ['bs', 'box-shadow', '0px 0px 0px 0px rgb(0, 0, 0)', '10px 20px 30px 4px rgb(200, 100, 0)'],
    ['bsnone', 'box-shadow', 'none', '10px 10px 10px red'],
    ['bspad', 'box-shadow', '2px 2px 0px 0px red', '4px 4px 0px 0px red, inset 10px 10px 10px 10px blue'],
    ['ts', 'text-shadow', '0px 0px 0px black', '10px 20px 4px white'],
    ['filt', 'filter', 'drop-shadow(0px 0px 0px black)', 'drop-shadow(10px 10px 4px red)'],
    ['bgpos', 'background-position', '0px 0px', '100px 50px'],
];
let html = '';
for (const [id, prop, from] of props)
    html += `<div id="${id}" style="width:10px;height:10px;${prop}:${from};transition:${prop} 1000ms linear"></div>`;
root.innerHTML = html;
flush();

const $ = (id) => document.getElementById(id);
for (const [id, prop, , to] of props) $(id).style.setProperty(prop, to);
flush();
advanceTime(500);

const cs = (id, prop) => getComputedStyle($(id)).getPropertyValue(prop);
// Compare the numbers in two value strings within a tolerance, with the
// words between them equal.
function sameValue(got, want, msg) {
    const split = (s) => s.split(/(-?[\d.]+(?:e-?\d+)?)/);
    const g = split(got), w = split(want);
    let ok = g.length === w.length;
    for (let i = 0; ok && i < g.length; i++) {
        if (i % 2) ok = Math.abs(+g[i] - +w[i]) < 1e-3;
        else ok = g[i].replace(/\s+/g, ' ') === w[i].replace(/\s+/g, ' ');
    }
    assert(ok, msg + ': expected ' + want + ', got ' + got);
}

// Different shapes: matrices. rotate(0) scale(1) → translateX(10px) scale(2).
sameValue(cs('m2d', 'transform'), 'matrix(1.5, 0, 0, 1.5, 5, 0)',
          'rotate(0) → translateX(10px) scale(2) blends through matrices');
// A rotation turns through the decomposition (a plain matrix blend would
// shrink it to 0.5, 0.5, -0.5, 0.5).
const h = Math.SQRT1_2;
sameValue(cs('rot', 'transform'), `matrix(${h}, ${h}, ${-h}, ${h}, 0, 5)`,
          'scale(1) → rotate(90deg) translateX(10px) turns 45 degrees');
// 3D: the rotation slerps, the scale blends (scale(2) leaves z at 1).
const s = 1.5 * h;
sameValue(cs('m3d', 'transform'),
          `matrix3d(1.5, 0, 0, 0, 0, ${s}, ${s}, 0, 0, ${-h}, ${h}, 0, 0, 0, 0, 1)`,
          'translateZ(0px) → rotateX(90deg) scale(2) blends through a 3D decomposition');
// A zero rotation has no axis of its own: the pair shares rotate3d.
sameValue(cs('axis', 'transform'), 'rotate3d(1, 0, 0, 45deg)',
          'rotateY(0deg) → rotateX(90deg) rotates about the other axis');
// The common prefix still blends as functions; the rest through a matrix.
sameValue(cs('prefix', 'transform'), 'translateX(50px) matrix(2, 0, 0, 2, 0, 0)',
          'the matching prefix blends per function, the rest as a matrix');
sameValue(cs('same', 'transform'), 'translateX(50px)', 'a same-shape pair keeps its functions');

sameValue(cs('bs', 'box-shadow'), '5px 10px 15px 2px rgb(100, 50, 0)',
          'every part of a box-shadow blends');
sameValue(cs('bsnone', 'box-shadow'), '5px 5px 5px 0px rgba(255, 0, 0, 0.5)',
          'none runs from a transparent shadow (premultiplied, so no darkening)');
sameValue(cs('bspad', 'box-shadow'), '3px 3px 0px 0px rgb(255, 0, 0), inset 5px 5px 5px 5px rgba(0, 0, 255, 0.5)',
          'the shorter shadow list is padded with a transparent shadow of the same kind');
sameValue(cs('ts', 'text-shadow'), '5px 10px 2px rgb(128, 128, 128)', 'every part of a text-shadow blends');
sameValue(cs('filt', 'filter'), 'drop-shadow(5px 5px 2px rgb(128, 0, 0))',
          'a filter function with a colour blends');
sameValue(cs('bgpos', 'background-position'), '50px 25px', 'a two-value length blends both');

advanceTime(600);
for (const [id, prop, , to] of props)
    assert($(id).getAnimations().length === 0, id + ': the transition ended');
sameValue(cs('m2d', 'transform'), 'translateX(10px) scale(2)', 'and ends on its value');
