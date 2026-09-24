// A `flex: 1` child of a stretched grid item fills the item as the row
// grows and shrinks. tools/reader's library cards: when a neighbouring card
// became taller, the card stretched with the row but its flex:1 progress
// fill kept its old height, because the grid's stretch re-layout wiped the
// stretched height it had preset (an item reused from last pass was claimed
// there for the first time) and the column flex saw an indefinite height.
// A row shrinking back left the fill at its stretched height the same way.

const root = document.getElementById('root');
root.innerHTML =
    '<div style="display:grid;grid-template-columns:300px 300px;gap:10px">' +
    '  <div id="a" style="display:flex;flex-direction:column;gap:8px;padding:10px">' +
    '    <div style="height:20px"></div>' +
    '    <div id="fill" style="flex:1;height:10px;overflow:hidden"></div>' +
    '    <div id="foot" style="height:20px"></div>' +
    '  </div>' +
    '  <div id="b" style="display:flex;flex-direction:column;padding:10px">' +
    '    <div id="grow" style="height:40px"></div>' +
    '  </div>' +
    '</div>' +
    // The same card in a stretching row flex container.
    '<div style="display:flex;align-items:stretch;margin-top:20px">' +
    '  <div id="c" style="width:300px;display:flex;flex-direction:column">' +
    '    <div id="cfill" style="flex:1;height:10px"></div>' +
    '  </div>' +
    '  <div id="cgrow" style="width:300px;height:50px"></div>' +
    '</div>';
flush();

const h = id => document.getElementById(id).getBoundingClientRect().height;
const top = id => document.getElementById(id).getBoundingClientRect().top;

function expectFill(what) {
    // a's content height is the row's less padding; the fill takes what the
    // two 20px rows and the two gaps leave.
    const content = h('a') - 20;
    const want = content - 20 - 20 - 16;
    assert(Math.abs(h('fill') - want) < 1,
           what + ': the fill takes the free space, want ' + want + ' got ' + h('fill'));
    assert(Math.abs((top('foot') + 20) - (top('a') + h('a') - 10)) < 1,
           what + ': the footer sits at the bottom of the card');
}

expectFill('initially');
const h0 = h('a');

document.getElementById('grow').style.height = '240px';
flush();
assert(h('a') > h0 + 150, 'the card stretched with its row, ' + h('a'));
expectFill('after the neighbour grew');

document.getElementById('grow').style.height = '100px';
flush();
expectFill('after it shrank part way');

document.getElementById('grow').style.height = '10px';
flush();
expectFill('after it shrank below the card');

// Row flex, cross-axis stretch.
assert(Math.abs(h('cfill') - 50) < 1, 'the flex item fill is 50 initially, got ' + h('cfill'));
document.getElementById('cgrow').style.height = '200px';
flush();
assert(Math.abs(h('c') - 200) < 1, 'the stretched flex item is 200, got ' + h('c'));
assert(Math.abs(h('cfill') - 200) < 1, 'and its fill follows, got ' + h('cfill'));
document.getElementById('cgrow').style.height = '80px';
flush();
assert(Math.abs(h('cfill') - 80) < 1, 'and follows it back down, got ' + h('cfill'));

root.innerHTML = '';
