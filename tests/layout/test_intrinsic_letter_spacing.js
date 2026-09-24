// Intrinsic widths count letter-spacing as layout does — one advance per
// character, not per UTF-8 byte — so a content-sized flex row holds all of its
// items; and a top-level inline-flex shrinks to its content.

const root = document.getElementById('root');
root.innerHTML =
    '<div style="display:flex;flex-direction:column;align-items:center">' +
    '<div id="row" style="display:flex;gap:7px">' +
    '<span id="dot" style="width:11px;height:11px;display:inline-block;background:red"></span>' +
    '<span>NAME</span><span id="wins" style="letter-spacing:2px">···</span>' +
    '</div></div>';
flush();

const r = (id) => document.getElementById(id).getBoundingClientRect();
assert(Math.abs(r('dot').width - 11) < 0.5, 'the dot keeps its 11px, got ' + r('dot').width);
assert(r('wins').right <= r('row').right + 0.5,
       'the letter-spaced item fits inside the row: ' + r('wins').right + ' vs ' + r('row').right);

// An inline-flex at the top level of a block is shrink-to-fit, like any
// atomic inline.
root.innerHTML = '<span id="chip" style="display:inline-flex">chip</span>' +
                 '<span id="grid" style="display:inline-grid;grid-template-columns:30px 50px"><i></i><i></i></span>';
flush();
assert(r('chip').width < 100, 'inline-flex is content-sized, got ' + r('chip').width);
assert(Math.abs(r('grid').width - 80) < 0.5, 'inline-grid is its tracks wide, got ' + r('grid').width);
assert(r('grid').left >= r('chip').right - 0.5, 'and they sit side by side on the line');

root.innerHTML = '';
