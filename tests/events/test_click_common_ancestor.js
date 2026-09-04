// A press and a release on two different elements is a click on the nearest
// element that contains both — the UI Events rule, and what makes a press on a
// button's icon that releases a pixel later on the button's own padding still a
// press of the button. It used to be a click only when the two were the same
// element, which dropped exactly that press.
//
// A press target that has left the tree by the release is not a click on
// anything, which is a browser's answer too: an application that rebuilds a
// button between the press and the release has replaced the thing that was
// pressed, and the release lands on a stranger.

const root = document.getElementById('root');
root.innerHTML =
    '<div id="outer" style="position:absolute;left:0;top:0;width:200px;height:100px;">' +
      '<button id="btn" style="position:absolute;left:0;top:0;width:100px;height:50px;padding:0;margin:0;border:0;">' +
        '<span id="icon" style="display:inline-block;width:20px;height:20px;"></span>' +
        '<span id="label">Go</span>' +
      '</button>' +
      '<div id="other" style="position:absolute;left:120px;top:0;width:50px;height:50px;"></div>' +
    '</div>';
flush();

const hits = [];
for (const id of ['outer', 'btn', 'icon', 'label', 'other'])
    document.getElementById(id).addEventListener('click', (e) => {
        if (e.currentTarget === e.target) hits.push(id);
    });

// Where the icon and the button's own box are, in window pixels.
const iconR = document.getElementById('icon').getBoundingClientRect();
const btnR = document.getElementById('btn').getBoundingClientRect();
const ix = iconR.left + iconR.width / 2, iy = iconR.top + iconR.height / 2;
const bx = btnR.left + btnR.width - 4, by = btnR.top + btnR.height - 4;   // past the spans
assert(document.elementFromPoint(ix, iy).id === 'icon', 'the icon is under the first point');
assert(document.elementFromPoint(bx, by).id === 'btn', 'the button itself is under the second');

// Press on the icon, release on the button: a click on the button.
mouseDown(ix, iy); flush();
mouseUp(bx, by); flush();
assert(hits.length === 1 && hits[0] === 'btn',
       `press on the icon, release on the button clicks the button (got ${JSON.stringify(hits)})`);

// Press on the icon, release on the label: the button contains both.
hits.length = 0;
const labelR = document.getElementById('label').getBoundingClientRect();
mouseDown(ix, iy); flush();
mouseUp(labelR.left + 2, labelR.top + labelR.height / 2); flush();
assert(hits.length === 1 && hits[0] === 'btn',
       `press on one span, release on the other clicks the button (got ${JSON.stringify(hits)})`);

// Press on the button, release on the sibling: the click goes to the parent
// they share, not to the button.
hits.length = 0;
mouseDown(bx, by); flush();
mouseUp(145, 25); flush();
assert(hits.length === 1 && hits[0] === 'outer',
       `press and release on siblings clicks their parent (got ${JSON.stringify(hits)})`);

// The same element still clicks itself.
hits.length = 0;
mouseDown(ix, iy); flush();
mouseUp(ix, iy); flush();
assert(hits.length === 1 && hits[0] === 'icon',
       `press and release on one element clicks it (got ${JSON.stringify(hits)})`);

// A press target rebuilt before the release is no click at all.
hits.length = 0;
mouseDown(ix, iy); flush();
const btn = document.getElementById('btn');
const fresh = btn.cloneNode(true);
btn.parentNode.replaceChild(fresh, btn);
flush();
fresh.addEventListener('click', () => hits.push('fresh'));
mouseUp(ix, iy); flush();
assert(hits.length === 0,
       `a press whose target was replaced before the release is not a click (got ${JSON.stringify(hits)})`);

root.innerHTML = '';
