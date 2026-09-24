// A flex item that is itself a flex (or grid) container with a percentage
// width: the parent row resolves the percentage once, and the item's own
// children are laid out against that width, not the percentage of it again.

const root = document.getElementById('root');
root.innerHTML =
    '<div style="display:flex;width:1000px">' +
    '<div id="col" style="width:40%;display:flex;flex-direction:column"><div id="c">x</div></div>' +
    '</div>' +
    '<div style="display:flex;width:1000px">' +
    '<div id="grid" style="width:50%;display:grid;grid-template-columns:1fr 1fr">' +
    '<div id="g1">a</div><div>b</div></div>' +
    '</div>';
flush();

const w = (id) => document.getElementById(id).getBoundingClientRect().width;
assert(Math.abs(w('col') - 400) < 0.5, 'column is 40% of the row, got ' + w('col'));
assert(Math.abs(w('c') - 400) < 0.5, 'its stretched child fills it, got ' + w('c'));
assert(Math.abs(w('grid') - 500) < 0.5, 'grid item is 50% of the row, got ' + w('grid'));
assert(Math.abs(w('g1') - 250) < 0.5, 'its 1fr track is half of that, got ' + w('g1'));

root.innerHTML = '';
