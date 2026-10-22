// table-layout: fixed sizes columns from <col> and the first row, never from
// cell content: a nowrap cell cannot widen a width:100% table, and its
// text-overflow: ellipsis then applies.

const root = document.getElementById('root');
root.innerHTML =
    '<div style="width:400px">' +
    '<table id="t" style="width:100%;table-layout:fixed;border-collapse:collapse">' +
    '<col style="width:100px"><col>' +
    '<tr><td id="c1">a</td>' +
    '<td id="c2" style="white-space:nowrap;overflow:hidden;text-overflow:ellipsis">' +
    'x'.repeat(200) + '</td></tr></table></div>';
flush();

const r = (id) => document.getElementById(id).getBoundingClientRect();
assert(Math.abs(r('t').width - 400) < 0.5, 'table keeps its 100% width, got ' + r('t').width);
assert(Math.abs(r('c1').width - 100) < 1, 'first column is the <col> width, got ' + r('c1').width);
assert(Math.abs(r('c2').width - 300) < 1, 'second column takes the rest, got ' + r('c2').width);
assert(inspect('#c2').indexOf('text-truncated') >= 0, 'the nowrap cell is ellipsized');

// ---- Percentage padding on table cell re-layouts cell children --------------
root.innerHTML =
    '<table id="t2" style="width:400px;border-collapse:collapse;border-spacing:0">' +
    '<tr><td id="padcell" style="padding:0 10%"><div id="inner" style="width:100%;height:20px"></div></td></tr>' +
    '</table>';
flush();
assert(Math.abs(r('padcell').width - 400) < 1, 'cell width is 400, got ' + r('padcell').width);
assert(Math.abs(r('inner').width - 320) < 1,
       'child inside 10% left/right padded cell re-laid out to 320px, got ' + r('inner').width);

// ---- thead / tbody / tfoot visual ordering & display:contents ---------------
root.innerHTML =
    '<table style="width:200px;border-collapse:collapse">' +
    '<tfoot><tr><td id="tf" style="height:20px">Foot</td></tr></tfoot>' +
    '<tbody><div style="display:contents"><tr><div style="display:contents"><td id="tb" style="height:20px">Body</td></div></tr></div></tbody>' +
    '<thead><tr><td id="th" style="height:20px">Head</td></tr></thead>' +
    '</table>';
flush();
assert(r('th').top < r('tb').top, 'thead laid out above tbody: ' + r('th').top + ' vs ' + r('tb').top);
assert(r('tb').top < r('tf').top, 'tbody laid out above tfoot: ' + r('tb').top + ' vs ' + r('tf').top);
assert(r('tb').width > 0 && r('tb').height >= 20, 'cell inside display:contents wrapper laid out');

root.innerHTML = '';
