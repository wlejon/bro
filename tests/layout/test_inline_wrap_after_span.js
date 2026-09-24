// A long text run inside an inline element wraps where it stands: after
// "voice " its first words stay on line 1 and only the rest wraps. As one
// atomic box the span moved whole to the next line, leaving line 1 with just
// "voice" on it.

const root = document.getElementById('root');
root.innerHTML =
    '<div id="d" style="width:300px;font:12px monospace;line-height:16px">' +
    '<span id="v">voice</span> <span id="m">recording did not start because the ' +
    'Steam library was not found anywhere</span></div>';
flush();

const d = document.getElementById('d').getBoundingClientRect();
const v = document.getElementById('v').getBoundingClientRect();
const m = document.getElementById('m');
const range = document.createRange();
range.selectNodeContents(m);
const first = range.getClientRects()[0];
assert(first, 'the span has client rects');
assert(Math.abs(first.top - v.top) < 1,
       'the span starts on the first line, got top ' + first.top + ' vs ' + v.top);
assert(first.left > v.right, 'after "voice", got left ' + first.left);
const rects = range.getClientRects();
const last = rects[rects.length - 1];
assert(last.top > first.top + 8, 'the rest wraps to a later line, got ' + last.top);
assert(Math.abs(d.bottom - last.bottom) < 4,
       'the block ends with the span\'s last line: ' + d.bottom + ' vs ' + last.bottom);

const mr = m.getBoundingClientRect();
assert(Math.abs(mr.top - v.top) < 1, 'the span box begins on line 1, got ' + mr.top);
assert(mr.bottom > v.bottom + 8, 'and ends further down, got ' + mr.bottom);

// Padding on a flattened inline takes room on its line; its text sits inside.
root.innerHTML =
    '<div style="font:16px monospace">a<span id="p" style="padding:0 10px">bc</span>d</div>';
flush();
const a0 = document.getElementById('p').getBoundingClientRect();
const pr = document.createRange();
pr.selectNodeContents(document.getElementById('p'));
const text = pr.getBoundingClientRect();
assert(Math.abs(text.left - (a0.left + 10)) < 1,
       'text sits inside the left padding: ' + text.left + ' vs ' + a0.left);

// An LTR run in an RTL paragraph stays one run across a span boundary:
// "abc def" reads left to right, the span's "abc" on the left.
const at = (html) => {
    root.innerHTML = html;
    flush();
    return document.getElementById('s').getBoundingClientRect().left;
};
const plain = at('<div style="font:16px monospace;width:300px" dir="rtl">' +
                 '<b id="s" style="display:inline-block">abc</b> def</div>');
const span = at('<div style="font:16px monospace;width:300px" dir="rtl">' +
                '<span id="s">abc</span> def</div>');
assert(Math.abs(plain - span) < 0.5,
       'a span reorders like any LTR text: ' + span + ' vs ' + plain);

root.innerHTML = '';
