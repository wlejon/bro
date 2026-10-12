// A text child of a flex container is an anonymous flex item laid out on its
// own line, so its white space is processed as one: runs collapse to a space
// and the line's leading and trailing spaces go (CSS Text §4.1). Source
// indentation around the text must not change where anything lands.
//
// Regression: the item was measured from its raw source text, so
// `Skip\n  <icon>` in a centred button measured "Skip" plus a newline and
// two spaces, and the icon sat ~9 px right of where `Skip<icon>` put it; with
// leading source whitespace the painted text also started a space late and
// ran into the icon.

const root = document.getElementById('root');
root.innerHTML =
    '<style>.b { display: flex; align-items: center; justify-content: center; gap: 6px;' +
    ' width: 120px; height: 34px; font: 13px sans-serif; }' +
    ' .b i { display: block; width: 14px; height: 14px; }</style>' +
    '<div id="tight" class="b">Skip<i></i></div>' +
    '<div id="loose" class="b">Skip\n  <i></i></div>' +
    '<div id="both" class="b">\n   Skip   it\n  <i></i></div>' +
    '<div id="two" class="b">Skip it<i></i></div>' +
    '<div id="pre" class="b" style="white-space: pre">Skip  <i></i></div>' +
    '<div id="pre1" class="b" style="white-space: pre">Skip<i></i></div>';
flush();

const iconX = (id) => document.querySelector('#' + id + ' i').getBoundingClientRect().left;

assert(Math.abs(iconX('loose') - iconX('tight')) < 0.01,
       'trailing source whitespace does not move the item: ' + iconX('loose') + ' vs ' + iconX('tight'));
assert(Math.abs(iconX('both') - iconX('two')) < 0.01,
       'leading whitespace and inner runs collapse as on a line of their own: ' + iconX('both') + ' vs ' + iconX('two'));
assert(iconX('pre') > iconX('pre1') + 1,
       'white-space: pre keeps its spaces: ' + iconX('pre') + ' vs ' + iconX('pre1'));

root.innerHTML = '';
