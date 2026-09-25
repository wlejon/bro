// `skew(ax, ay)` is the single matrix [1 tan(ax); tan(ay) 1] (CSS
// Transforms 1), not skewY(ay)·skewX(ax), whose product also stretches y by
// 1 + tan(ax)·tan(ay).
//
// A 100x100 box under skew(45deg, 30deg) about its centre maps its corners
// (±50, ±50) to x = x + y and y = tan30·x + y: 200 wide, 100 + 100·tan30
// (157.7) tall. The composed form [1 tan45; tan30 1+tan45·tan30] made it
// 100 + 200·tan30 (215.5) tall. getBoundingClientRect reads the 3D parse, as does the transition
// interpolator when it has to blend two lists through their matrices.

const T30 = Math.tan(Math.PI / 6);
document.body.style.cssText = 'margin:0;';
document.body.innerHTML =
    '<div id="a" style="position:absolute;left:200px;top:200px;width:100px;height:100px;' +
    'transform:skew(45deg, 30deg);"></div>' +
    // Two lists of different shapes blend through matrix decomposition; both
    // ends are the same matrix, so every point of the transition is too.
    '<div id="b" style="position:absolute;left:500px;top:200px;width:100px;height:100px;' +
    'transform:skew(45deg, 30deg);transition:transform 1000ms linear;"></div>';
flush();

function checkRect(id, when) {
    const r = document.getElementById(id).getBoundingClientRect();
    assert(Math.abs(r.width - 200) < 0.5, id + ' ' + when + ' is 200 wide, got ' + r.width);
    const h = 100 + 100 * T30;
    assert(Math.abs(r.height - h) < 0.5, id + ' ' + when + ' is ' + h.toFixed(1) +
           ' tall, got ' + r.height);
}
checkRect('a', 'skewed');
checkRect('b', 'before the transition');

document.getElementById('b').style.transform = 'matrix(1, ' + T30 + ', 1, 1, 0, 0)';
flush();
advanceTime(500);
checkRect('b', 'halfway to the equal matrix()');
