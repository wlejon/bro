// A <button>'s content sits in an anonymous box centred in the block
// dimension when the button is taller than it (HTML §15.5 button layout), as
// a height: 32px "Save" button reads in its middle in every browser (a flex
// or grid button lays out its own items). Content moved this way (and by a
// table cell's vertical-align) moves once: text inside a <span> goes with
// the span, not twice.
//
// Regression: the label sat at the top of a fixed-height button (2 px below
// its border), and a vertically centred cell drew a span's text a second
// shift lower, on top of the next line.

const root = document.getElementById('root');
root.innerHTML =
    '<style>.b { height: 40px; font: 13px sans-serif; line-height: 15px; }' +
    ' td { height: 60px; vertical-align: middle; font: 13px sans-serif; line-height: 15px; }</style>' +
    '<button id="ib" class="b">Save</button>' +
    '<div style="display:flex"><button id="fi" class="b">Keep</button></div>' +
    '<button id="bl" class="b" style="display:block">Block</button>' +
    '<button id="two" style="height:60px; font: 13px sans-serif; line-height: 15px"><span id="sp">two</span><br>lines</button>' +
    '<table><tr><td id="td"><span id="tsp">two</span><br>lines</td></tr></table>';
flush();

const box = (el) => el.getBoundingClientRect();
const textRect = (node) => { const r = document.createRange(); r.selectNodeContents(node); return r.getBoundingClientRect(); };
// Gap above the label minus gap below: 0 when centred.
const skew = (id) => {
    const el = document.getElementById(id), b = box(el), t = textRect(el.firstChild);
    return (t.top - b.top) - (b.bottom - t.bottom);
};

for (const id of ['ib', 'fi', 'bl'])
    assert(Math.abs(skew(id)) <= 1, id + ': a fixed-height button centres its label: skew ' + skew(id));

for (const [cell, span] of [['two', 'sp'], ['td', 'tsp']]) {
    const s = document.getElementById(span);
    const lines = document.getElementById(cell).lastChild;
    const words = textRect(s.firstChild), last = textRect(lines);
    assert(Math.abs(words.top - box(s).top) <= 1,
           cell + ': the span\'s text moves with the span, once: ' + words.top + ' vs ' + box(s).top);
    assert(last.top >= words.bottom - 1, cell + ': the two lines do not overlap: ' + words.bottom + ' / ' + last.top);
    const b = box(document.getElementById(cell));
    const skew2 = (words.top - b.top) - (b.bottom - last.bottom);
    assert(Math.abs(skew2) <= 3, cell + ': two lines centred: skew ' + skew2);
}

root.innerHTML = '';
