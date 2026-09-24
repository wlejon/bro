// HTMLTextAreaElement / HTMLInputElement .setRangeText() — it did not exist
// (typeof was 'undefined'), so editors spliced `value` by hand.

const root = document.getElementById('root');
root.innerHTML = '<textarea id="ta">hello world</textarea><input id="in" value="abcdef">' +
                 '<div id="d"></div>';
flush();
const ta = document.getElementById('ta');
const inp = document.getElementById('in');

assert(typeof ta.setRangeText === 'function', 'textarea.setRangeText exists');
assert(typeof inp.setRangeText === 'function', 'input.setRangeText exists');

// One argument: replaces the selection; "preserve" keeps a selection that
// covered the replaced span over the replacement.
ta.setSelectionRange(6, 11);
ta.setRangeText('there');
assert(ta.value === 'hello there', 'replaces the selection (' + ta.value + ')');
assert(ta.selectionStart === 6 && ta.selectionEnd === 11,
    'preserve: selection spans the replacement (' + ta.selectionStart + ',' + ta.selectionEnd + ')');

// Explicit range + each selection mode.
ta.value = 'hello world';
ta.setRangeText('big ', 6, 6, 'end');
assert(ta.value === 'hello big world', 'insert at a point (' + ta.value + ')');
assert(ta.selectionStart === 10 && ta.selectionEnd === 10, 'end: caret after the insertion');

ta.setRangeText('BIG', 6, 9, 'select');
assert(ta.value === 'hello BIG world', 'replace a span (' + ta.value + ')');
assert(ta.selectionStart === 6 && ta.selectionEnd === 9, 'select: the replacement is selected');

ta.setRangeText('', 5, 9, 'start');
assert(ta.value === 'hello world', 'delete a span (' + ta.value + ')');
assert(ta.selectionStart === 5 && ta.selectionEnd === 5, 'start: caret at the start');

// preserve with the selection after the edit shifts by the length change.
ta.value = 'one two three';
ta.setSelectionRange(8, 13);          // "three"
ta.setRangeText('2', 4, 7);           // "two" -> "2"
assert(ta.value === 'one 2 three', 'edit before the selection (' + ta.value + ')');
assert(ta.selectionStart === 6 && ta.selectionEnd === 11,
    'preserve: a later selection shifts left (' + ta.selectionStart + ',' + ta.selectionEnd + ')');

// UTF-16 offsets.
ta.value = 'a\u{1F600}b';
ta.setRangeText('X', 3, 4, 'end');
assert(ta.value === 'a\u{1F600}X', 'offsets count UTF-16 units (' + ta.value + ')');
assert(ta.selectionStart === 4, 'caret after X in UTF-16 units (' + ta.selectionStart + ')');

// <input>.
inp.setRangeText('XY', 2, 4, 'select');
assert(inp.value === 'abXYef', 'input value (' + inp.value + ')');
assert(inp.selectionStart === 2 && inp.selectionEnd === 4, 'input selection');

// Errors.
let name = null;
try { ta.setRangeText('x', 5, 2); } catch (e) { name = e.name; }
assert(name === 'IndexSizeError', 'start > end throws IndexSizeError (' + name + ')');
name = null;
try { ta.setRangeText('x', 0, 1, 'bogus'); } catch (e) { name = e.name; }
assert(name === 'TypeError', 'an unknown selectionMode throws TypeError (' + name + ')');

// No input event: it is a programmatic change.
let inputs = 0;
ta.addEventListener('input', () => inputs++);
ta.setRangeText('z', 0, 0);
advanceTime(20);
flush();
assert(inputs === 0, 'no input event');

console.log('test_set_range_text: done');
