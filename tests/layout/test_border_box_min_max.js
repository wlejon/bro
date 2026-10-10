// min-width / max-width / min-height / max-height name the BORDER box under
// box-sizing: border-box, for block, flex and grid boxes alike. A list row with
// `min-height: 52px; padding: 8px 0; box-sizing: border-box` is 52 tall; it was
// 68, the min-height applied to the content box and the padding added on top.
// A row flex container also centres its items in that 52px box.

const root = document.getElementById('root');
const near = (a, b) => Math.abs(a - b) < 0.5;

for (const display of ['block', 'flex', 'grid']) {
  root.innerHTML =
    '<div id="row" style="display:' + display + ';box-sizing:border-box;min-height:52px;' +
    'padding:8px 0;border-top:1px solid;align-items:center"><span id="s" style="display:block;height:20px;width:30px"></span></div>' +
    '<div id="cap" style="display:' + display + ';box-sizing:border-box;height:100px;max-height:30px;' +
    'width:10px;min-width:100px;padding:8px 10px"><span style="display:block;height:5px"></span></div>' +
    '<div id="wide" style="display:' + display + ';box-sizing:border-box;max-width:200px;' +
    'padding:0 10px;border-left:5px solid">x</div>' +
    '<div id="cb" style="display:' + display + ';box-sizing:content-box;min-height:52px;padding:8px 0">x</div>';
  const rect = (id) => document.getElementById(id).getBoundingClientRect();
  assert(near(rect('row').height, 52),
         display + ': border-box min-height is the border box, got ' + rect('row').height);
  assert(near(rect('cap').height, 30),
         display + ': border-box max-height is the border box, got ' + rect('cap').height);
  assert(near(rect('cap').width, 100),
         display + ': border-box min-width is the border box, got ' + rect('cap').width);
  assert(near(rect('wide').width, 200),
         display + ': border-box max-width is the border box, got ' + rect('wide').width);
  assert(near(rect('cb').height, 68),
         display + ': content-box min-height is still the content box, got ' + rect('cb').height);
  if (display === 'flex') {
    // 1px border + 8px padding, then centred in the 35px content box.
    const s = rect('s'), r = rect('row');
    assert(near(s.y - r.y, 1 + 8 + (35 - 20) / 2),
           'flex: the item is centred in the min-height box, offset ' + (s.y - r.y));
  }
}

root.innerHTML = '';
