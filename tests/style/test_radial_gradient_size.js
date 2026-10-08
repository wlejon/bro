// radial-gradient() honours an explicit size: one <length> is a circle's
// radius, two <length-percentage>s an ellipse's (percentages against the
// gradient box). The prefix parser only knew shape/extent keywords and `at`,
// so `radial-gradient(400px 300px at 20% 30%, ...)` drew farthest-corner, and
// a size with no `at` was not recognised as a prefix at all: it was parsed as
// a colour stop, painting black.

document.body.style.cssText = 'margin:0;background:rgb(0,0,0)';

// Red at the centre to blue at the end radius: the red channel reads how far
// along the ray a pixel is (255 at 0, 0 at the radius and beyond).
function redAt(bg, points) {
    document.body.innerHTML =
        `<div id="b" style="position:absolute;left:0;top:0;width:400px;height:300px;background:${bg}"></div>`;
    flush();
    return points.map(([x, y]) => getPixel(x, y));
}
const near = (v, want, tol, what) =>
    assert(Math.abs(v - want) <= tol, `${what}: red ${v}, expected ~${want}`);

// Ellipse 100 x 50 around the box centre (200, 150).
let p = redAt('radial-gradient(100px 50px at 50% 50%, rgb(255,0,0), rgb(0,0,255))',
              [[250, 150], [299, 150], [320, 150], [200, 175], [200, 205]]);
near(p[0].r, 128, 12, 'ellipse 100x50: halfway along x');
near(p[1].r, 3, 8, 'ellipse 100x50: at rx');
near(p[2].r, 0, 2, 'ellipse 100x50: past rx');
near(p[3].r, 128, 12, 'ellipse 100x50: halfway along y');
near(p[4].r, 0, 2, 'ellipse 100x50: past ry');

// The same with no `at`: the size alone is the prefix, centred.
let q = redAt('radial-gradient(100px 50px, rgb(255,0,0), rgb(0,0,255))', [[200, 150], [250, 150], [200, 175]]);
assert(q[0].r > 240 && q[0].b < 16, `size-only prefix: centre is the first stop, got rgb(${q[0].r},${q[0].g},${q[0].b})`);
near(q[1].r, 128, 12, 'size-only prefix: halfway along x');
near(q[2].r, 128, 12, 'size-only prefix: halfway along y');

// One length is a circle.
p = redAt('radial-gradient(100px at center, rgb(255,0,0), rgb(0,0,255))', [[250, 150], [200, 200]]);
near(p[0].r, 128, 12, 'circle 100px: halfway along x');
near(p[1].r, 128, 12, 'circle 100px: halfway along y');
p = redAt('radial-gradient(circle 60px at 100px 75px, rgb(255,0,0), rgb(0,0,255))', [[100, 75], [130, 75], [100, 140]]);
assert(p[0].r > 240, `circle at a px position: centre red, got ${p[0].r}`);
near(p[1].r, 128, 12, 'circle 60px at 100px 75px: halfway');
near(p[2].r, 0, 2, 'circle 60px at 100px 75px: past the radius');

// Percentages against the box: 50% x 25% of 400 x 300 = 200 x 75.
p = redAt('radial-gradient(50% 25% at 50% 50%, rgb(255,0,0), rgb(0,0,255))', [[300, 150], [200, 187]]);
near(p[0].r, 128, 12, 'ellipse 50% 25%: halfway along x (100 of 200px)');
near(p[1].r, 128, 14, 'ellipse 50% 25%: halfway along y (37 of 75px)');

// The issue's form: 400px 300px at 20% 30% of the box = centre (80, 90).
p = redAt('radial-gradient(400px 300px at 20% 30%, rgb(255,0,0), rgb(0,0,255))', [[80, 90], [280, 90], [80, 240]]);
assert(p[0].r > 240, `400px 300px at 20% 30%: centre red, got ${p[0].r}`);
near(p[1].r, 128, 12, '400px 300px: halfway along x');
near(p[2].r, 128, 12, '400px 300px: halfway along y');

// Extent keywords still work.
p = redAt('radial-gradient(closest-side, rgb(255,0,0), rgb(0,0,255))', [[300, 150], [200, 225]]);
near(p[0].r, 128, 12, 'closest-side ellipse: halfway along x (100 of 200px)');
near(p[1].r, 128, 12, 'closest-side ellipse: halfway along y (75 of 150px)');

console.log('radial-gradient sizes OK');
