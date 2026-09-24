// Inline elements wrap where they stand in every wrapping mode, not only
// white-space: normal: pre-wrap text, overflow-wrap: break-word text, and the
// inline runs of a block that also holds block-level children. Each case puts
// a long span after a short word on a line too narrow for both; the span has
// to start on the first line, after the word, and wrap from there. And the
// padding of a right-to-left span in a right-to-left paragraph mirrors.

const root = document.getElementById('root');

function check(label, html) {
    root.innerHTML = html;
    flush();
    const v = document.getElementById('v').getBoundingClientRect();
    const m = document.getElementById('m');
    const range = document.createRange();
    range.selectNodeContents(m);
    const rects = range.getClientRects();
    assert(rects.length > 0, label + ': the span has client rects');
    const first = rects[0];
    assert(Math.abs(first.top - v.top) < 1,
           label + ': the span starts on the first line, got top ' + first.top + ' vs ' + v.top);
    assert(first.left > v.right - 0.5, label + ': after "voice", got left ' + first.left);
    const last = rects[rects.length - 1];
    assert(last.top > first.top + 8, label + ': the rest wraps to a later line, got ' + last.top);
    const mr = m.getBoundingClientRect();
    assert(Math.abs(mr.top - v.top) < 1, label + ': the span box begins on line 1, got ' + mr.top);
}

const text = 'recording did not start because the Steam library was not found anywhere';

check('pre-wrap',
      '<div style="width:300px;font:12px monospace;line-height:16px;white-space:pre-wrap">' +
      '<span id="v">voice</span> <span id="m">' + text + '</span></div>');

check('break-word',
      '<div style="width:300px;font:12px monospace;line-height:16px;overflow-wrap:break-word">' +
      '<span id="v">voice</span> <span id="m">' + text + '</span></div>');

check('mixed block and inline',
      '<div style="width:300px;font:12px monospace;line-height:16px">' +
      '<div>a block first</div>' +
      '<span id="v">voice</span> <span id="m">' + text + '</span></div>');

// pre-wrap keeps its spaces and breaks after them: a line never starts with
// the space it broke at.
root.innerHTML =
    '<div id="pw" style="width:120px;font:12px monospace;line-height:16px;white-space:pre-wrap">' +
    'aaaa bbbb cccc dddd eeee ffff</div>';
flush();
const pw = document.getElementById('pw');
const pr = document.createRange();
pr.selectNodeContents(pw);
const lines = pr.getClientRects();
const box = pw.getBoundingClientRect();
assert(lines.length >= 2, 'pre-wrap text wraps, got ' + lines.length + ' rects');
for (let i = 0; i < lines.length; ++i) {
    assert(lines[i].left >= box.left - 0.5, 'no rect starts left of the box');
}
assert(Math.abs(box.height - 32) < 1, 'two lines of pre-wrap text, got ' + box.height);

// break-word cuts a word longer than the line at the line's edge.
root.innerHTML =
    '<div id="bw" style="width:100px;font:12px monospace;line-height:16px;overflow-wrap:break-word">' +
    'xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx</div>';
flush();
const bw = document.getElementById('bw').getBoundingClientRect();
assert(bw.height >= 48 - 1, 'a 40-character word in 100px breaks onto several lines, got ' + bw.height);
const br = document.createRange();
br.selectNodeContents(document.getElementById('bw'));
for (const r of br.getClientRects())
    assert(r.right <= bw.right + 0.5, 'no piece overflows the box: ' + r.right + ' vs ' + bw.right);

// A right-to-left span in a right-to-left paragraph: padding-left on the left
// of its text, padding-right on the right.
root.innerHTML =
    '<div dir="rtl" style="width:300px;font:16px monospace">' +
    '<span id="s" style="padding-left:5px;padding-right:11px">שלום עולם</span></div>';
flush();
const s = document.getElementById('s');
const sr = s.getBoundingClientRect();
const tr = document.createRange();
tr.selectNodeContents(s);
const tb = tr.getBoundingClientRect();
assert(Math.abs(tb.left - sr.left - 5) < 1,
       'padding-left is on the left: ' + (tb.left - sr.left));
assert(Math.abs(sr.right - tb.right - 11) < 1,
       'padding-right is on the right: ' + (sr.right - tb.right));

root.innerHTML = '';
