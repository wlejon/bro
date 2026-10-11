// A right press-and-release is not a click (UI Events): it is an auxclick and
// then a contextmenu, and it takes no part in double-click counting.
//
// Three things went wrong before. The engine fired click for the right button,
// and counted it in the double-click run, so a left click on a list row
// followed by a right click on the same row was a dblclick: a file list opened
// the folder it was asked for a context menu on. A right press toggled a
// checkbox it never clicked. And contextmenu went to the target hit before
// mouseup, so a list that repaints its row on mouseup sent the event into the
// detached old row, where the list listening for it never heard it.

const root = document.getElementById('root');
root.innerHTML =
    '<div id="list" style="position:absolute;left:0;top:0;width:300px;height:100px">' +
    '  <div id="row" style="height:30px"><span id="cell">row</span></div></div>' +
    '<input id="box" type="checkbox" style="position:absolute;left:0;top:120px;width:20px;height:20px">';
flush();

const list = document.getElementById('list');
const seen = [];
for (const t of ['click', 'auxclick', 'dblclick', 'contextmenu']) {
    list.addEventListener(t, (e) => seen.push(`${t}:${e.button}`));
}
// Repaint the row on a right mouseup, as a virtual list does when the press
// moved the selection. (On a left one the click would then be on nothing,
// which is the browser's answer and test_click_common_ancestor's subject.)
let repaint = false;
list.addEventListener('mouseup', (e) => {
    if (!repaint || e.button !== 2) return;
    list.innerHTML = '<div id="row" style="height:30px"><span id="cell">row</span></div>';
});

const at = (id) => {
    const b = document.getElementById(id).getBoundingClientRect();
    return [(b.left + b.width / 2) | 0, (b.top + b.height / 2) | 0];
};

// --- left, then right, on the same spot -------------------------------------
let [x, y] = at('cell');
click(x, y, 0);
flush();
[x, y] = at('cell');
click(x, y, 2);
flush();
assert(seen.join(' ') === 'click:0 auxclick:2 contextmenu:2',
    `left then right: click, auxclick, contextmenu, no dblclick: ${seen.join(' ')}`);

// --- right, then left, is a single click, not a double -----------------------
seen.length = 0;
[x, y] = at('cell');
click(x, y, 0);
flush();
assert(seen.join(' ') === 'click:0', `the left click after a right is the first of a run: ${seen.join(' ')}`);

// --- two lefts still make a dblclick -----------------------------------------
seen.length = 0;
[x, y] = at('cell');
click(x, y, 0);
flush();
assert(seen.includes('dblclick:0'), `two left clicks are still a dblclick: ${seen.join(' ')}`);

// --- the contextmenu reaches a listener above a row the mouseup replaced ------
// The press target is gone by the release, so there is no auxclick (a
// click on nothing), but the contextmenu still comes.
repaint = true;
seen.length = 0;
let menuTarget = null;
document.addEventListener('contextmenu', (e) => { menuTarget = e.target; });
[x, y] = at('cell');
click(x, y, 2);
flush();
assert(seen.join(' ') === 'contextmenu:2', `the list hears the contextmenu: ${seen.join(' ')}`);
assert(menuTarget && menuTarget.isConnected, 'contextmenu is on an element in the document');
assert(list.contains(menuTarget), 'and on what stands under the pointer now');
repaint = false;

// --- a right press does not toggle a checkbox ---------------------------------
const box = document.getElementById('box');
[x, y] = at('box');
click(x, y, 2);
flush();
assert(!box.checked, 'right-clicking a checkbox leaves it unchecked');
click(x, y, 0);
flush();
assert(box.checked, 'a left click checks it');

