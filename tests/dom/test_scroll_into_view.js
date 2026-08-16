// Element.scrollIntoView(): scroll the scrollable ancestors so the element is
// visible. Vertical only — element-level horizontal scrolling does not exist.

const root = document.getElementById('root');

let rows = '';
for (let i = 0; i < 40; i++) rows += '<div id="r' + i + '" style="height:30px">row ' + i + '</div>';
root.innerHTML = '<div id="box" style="height:120px;overflow:auto">' + rows + '</div>';
flush();

const box = document.getElementById('box');
assert(box.scrollTop === 0, 'starts unscrolled');

// Default alignment is "start": the element's top meets the container's top.
document.getElementById('r20').scrollIntoView();
flush();
let br = box.getBoundingClientRect();
let tr = document.getElementById('r20').getBoundingClientRect();
assert(box.scrollTop > 0, 'scrollIntoView scrolled the container');
assert(Math.abs(tr.top - br.top) <= 3, 'start-aligned: r20 top at the container top');

// "nearest" leaves an already-visible element alone.
const held = box.scrollTop;
document.getElementById('r21').scrollIntoView({ block: 'nearest' });
flush();
assert(box.scrollTop === held, 'nearest does not move a visible element');

// "end" puts the element's bottom at the container's bottom.
document.getElementById('r30').scrollIntoView({ block: 'end' });
flush();
br = box.getBoundingClientRect();
tr = document.getElementById('r30').getBoundingClientRect();
assert(Math.abs(tr.bottom - br.bottom) <= 4, 'end-aligned: r30 bottom at the container bottom');

// "center" splits the difference.
document.getElementById('r10').scrollIntoView({ block: 'center' });
flush();
br = box.getBoundingClientRect();
tr = document.getElementById('r10').getBoundingClientRect();
assert(Math.abs((tr.top + tr.height / 2) - (br.top + br.height / 2)) <= 4,
       'center-aligned: r10 centered in the container');

// scrollIntoView(false) is the legacy spelling of block: "end".
box.scrollTop = 0;
flush();
document.getElementById('r15').scrollIntoView(false);
flush();
br = box.getBoundingClientRect();
tr = document.getElementById('r15').getBoundingClientRect();
assert(Math.abs(tr.bottom - br.bottom) <= 4, 'scrollIntoView(false) aligns to the end');

// A container that does not clip is not scrolled.
root.innerHTML = '<div id="plain" style="height:120px">' + rows + '</div>';
flush();
document.getElementById('r20').scrollIntoView();
flush();
assert(document.getElementById('plain').scrollTop === 0,
       'overflow:visible container is left alone');

root.innerHTML = '';
