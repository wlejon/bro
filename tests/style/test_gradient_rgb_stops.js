// linear-gradient() stops written with rgb()/hsl() paint their colours. The
// stop parser peels trailing position tokens off each stop; it used to split
// at the spaces inside `rgb(20, 40, 90)`, took "90)" for a position, and the
// shredded colour fell back to black.

const root = document.getElementById('root');
root.innerHTML =
    '<div id="g1" style="width:200px;height:20px;background:linear-gradient(90deg, rgb(200, 0, 0), rgb(0, 0, 200))"></div>' +
    '<div id="g2" style="width:200px;height:20px;background-image:linear-gradient(90deg, rgb(0 200 0) 0%, hsl(240, 100%, 40%) 100%)"></div>';
flush();

function px(id, fx) {
    const r = document.getElementById(id).getBoundingClientRect();
    return getPixel(Math.floor(r.left + r.width * fx), Math.floor(r.top + r.height / 2));
}

let p = px('g1', 0.02);
assert(p.r > 150 && p.b < 60, `rgb() first stop paints red, got rgb(${p.r},${p.g},${p.b})`);
p = px('g1', 0.98);
assert(p.b > 150 && p.r < 60, `rgb() last stop paints blue, got rgb(${p.r},${p.g},${p.b})`);
p = px('g2', 0.02);
assert(p.g > 150 && p.r < 60, `space-separated rgb() stop with a position paints green, got rgb(${p.r},${p.g},${p.b})`);
p = px('g2', 0.98);
assert(p.b > 120 && p.g < 60, `hsl() stop with a position paints blue, got rgb(${p.r},${p.g},${p.b})`);
