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

root.innerHTML = '';
