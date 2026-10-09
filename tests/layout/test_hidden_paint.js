// What is invisible paints nothing: text under visibility:hidden (the text
// node takes its parent's computed visibility, which inherits), the backdrop
// filter of a hidden element, and a whole opacity:0 subtree. A visible
// descendant of a hidden element still paints, as CSS has it.
//
// A shell hides its bar with visibility:hidden over a fullscreen window; any
// of these painting would put pixels over the window (and keep the frame from
// being the window's buffer alone).

document.body.style.cssText = 'margin:0;background:#000;';
const big = 'font:bold 40px/48px sans-serif;color:#fff;';
document.body.innerHTML =
    // Hidden text, white on black: none of it may show.
    '<div id="hid" style="position:absolute;left:10px;top:10px;width:300px;height:48px;visibility:hidden;' + big + '">' +
    '████████</div>' +
    // A visible child inside a hidden parent shows.
    '<div style="position:absolute;left:10px;top:70px;width:300px;height:48px;visibility:hidden;' + big + '">' +
    '<span id="vis" style="visibility:visible">████████</span></div>' +
    // A hidden element's backdrop filter (invert over black would be white).
    '<div id="bf" style="position:absolute;left:10px;top:130px;width:100px;height:40px;' +
    'visibility:hidden;backdrop-filter:invert(1);"></div>' +
    // A faded-out overlay: its background and its text stay off the screen.
    '<div id="fade" style="position:absolute;left:10px;top:190px;width:300px;height:48px;opacity:0;' +
    'background:#f00;' + big + '"><span style="opacity:1">████████</span></div>';
flush();

function px(x, y) { return getPixel(Math.round(x), Math.round(y)); }
function isBlack(p) { return p.r < 16 && p.g < 16 && p.b < 16; }
function rgb(p) { return 'rgb(' + p.r + ',' + p.g + ',' + p.b + ')'; }

// Sample across a box's middle row; true if any sample is not black.
function anyPainted(r) {
    for (let x = r.left + 2; x < r.right - 2; x += 4)
        if (!isBlack(px(x, r.top + r.height / 2))) return true;
    return false;
}

const hid = document.getElementById('hid').getBoundingClientRect();
assert(!anyPainted(hid), 'text under visibility:hidden is not painted');

const vis = document.getElementById('vis').getBoundingClientRect();
assert(anyPainted(vis), 'a visibility:visible descendant of a hidden element still paints');

const bf = document.getElementById('bf').getBoundingClientRect();
const bfp = px(bf.left + 50, bf.top + 20);
assert(isBlack(bfp), 'a hidden element\'s backdrop filter is not drawn, got ' + rgb(bfp));

const fade = document.getElementById('fade').getBoundingClientRect();
assert(!anyPainted(fade), 'an opacity:0 subtree is not painted');

// Shown again, each paints.
document.getElementById('hid').style.visibility = 'visible';
document.getElementById('fade').style.opacity = '1';
flush();
assert(anyPainted(hid), 'the text paints once visible');
const fp = px(fade.right - 3, fade.top + 3);
assert(fp.r > 150 && fp.g < 60, 'the overlay paints at opacity 1, got ' + rgb(fp));
