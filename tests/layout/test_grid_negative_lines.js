// Negative grid line numbers count back from the end of the explicit grid:
// `grid-column: 1 / -1` spans every column, `-2 / -1` is the last one.

const root = document.getElementById('root');
root.innerHTML =
    '<div style="display:grid;grid-template-columns:1fr 1fr;width:400px">' +
    '<div id="a">a</div><div>b</div>' +
    '<div id="full" style="grid-column:1 / -1">full</div>' +
    '<div id="last" style="grid-column:-2 / -1">last</div>' +
    '</div>';
flush();

const r = (id) => document.getElementById(id).getBoundingClientRect();
assert(Math.abs(r('a').width - 200) < 0.5, 'a plain item is one column, got ' + r('a').width);
assert(Math.abs(r('full').left) < 0.5 && Math.abs(r('full').width - 400) < 0.5,
       '1 / -1 spans both columns, got ' + r('full').left + ' / ' + r('full').width);
assert(Math.abs(r('last').left - 200) < 0.5 && Math.abs(r('last').width - 200) < 0.5,
       '-2 / -1 is the last column, got ' + r('last').left + ' / ' + r('last').width);

root.innerHTML = '';
