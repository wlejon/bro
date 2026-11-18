// SVG text positions take lengths: dy="0.71em" moves the text by 0.71 of its
// font size, as d3's axis labels need (Chromium: the label's box top moves
// from y 20 to 27.1 at font-size 10).
document.body.style.margin = '0';
document.body.innerHTML =
    '<svg width="300" height="100" font-family="sans-serif" font-size="10"><g transform="translate(50,20)">' +
    '<text id="a" y="9" dy="0.71em">A</text><text id="b" x="30" y="9">B</text>' +
    '<text id="c" x="60" y="9" dy="7px">C</text><text id="d" x="90" y="0.9em" dx="1em">D</text></g></svg>';
const top = (id) => document.getElementById(id).getBoundingClientRect();
const a = top('a'), b = top('b'), c = top('c'), d = top('d');
assert(Math.abs((a.y - b.y) - 7.1) < 0.3, `dy=0.71em moves 7.1px: ${a.y - b.y}`);
assert(Math.abs(a.y - c.y) < 0.3, `0.71em at 10px is 7px-ish: ${a.y} vs ${c.y}`);
assert(Math.abs(d.y - b.y) < 0.3 && Math.abs(d.x - (50 + 90 + 10)) < 0.5, `y=0.9em and dx=1em: ${d.x},${d.y}`);
console.log('test_svg_text_em_offsets: OK');
