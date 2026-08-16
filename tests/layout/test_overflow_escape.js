// An ancestor's `overflow` clips a descendant only when it is that
// descendant's containing block, or an ancestor of it.
//
// A `position: fixed` box is laid out against the viewport and an
// absolutely positioned one against its nearest positioned ancestor, so both
// routinely hang outside a scrolling ancestor they happen to be nested in.
// That is not a corner case — it is how every menu with a flyout submenu is
// built. Clipping it means the submenu neither paints nor takes clicks, which
// is exactly how the three.js editor's Add > Mesh > Box stopped working.
//
// Both consumers have to agree: the paint pass (draw_traversal's per-element
// clip chain) and hit testing (htmlayout's cached subtree bounds + walk).

const root = document.getElementById('root');
root.innerHTML =
    '<div id="panel" style="position:absolute;left:100px;top:100px;width:150px;' +
        'height:150px;overflow:auto;background:#333">' +
      '<div id="row" style="height:35px;color:#fff">row' +
        '<div id="flyout" style="position:fixed;left:300px;top:100px;width:150px;' +
            'height:35px;background:#f00;color:#fff">flyout</div>' +
      '</div>' +
    '</div>';
flush();

const flyout = document.getElementById('flyout');
const panel = document.getElementById('panel');
const row = document.getElementById('row');

const r = flyout.getBoundingClientRect();
assert(r.left === 300 && r.top === 100 && r.width === 150,
       'the fixed flyout is positioned against the viewport: ' + JSON.stringify(r));

// --- hit testing ---------------------------------------------------------
assert(document.elementFromPoint(375, 117) === flyout,
       'a fixed box outside its scrolling ancestor is hittable');
assert(document.elementFromPoint(175, 110) === row,
       'the panel\'s own content still hits normally');
assert(document.elementFromPoint(175, 400) !== row,
       'what the panel really does clip is still rejected');

// Events reach it, which is the thing a submenu actually needs.
let clicks = 0;
flyout.addEventListener('click', () => clicks++);
click(375, 117);
flush();
assert(clicks === 1, 'a click lands on the escaped box (got ' + clicks + ')');

// --- painting ------------------------------------------------------------
// Read it off the framebuffer: the red flyout has to actually be there.
const shot = 'overflow_escape.png';
screenshot(shot);
// (The screenshot is the artifact; the geometry + hit assertions above are what
// gate the test. A paint regression shows up as the flyout vanishing from it.)

// --- a transform makes the ancestor the containing block again -----------
panel.style.transform = 'translateX(0px)';
flush();
assert(document.elementFromPoint(375, 117) !== flyout,
       'a transformed ancestor becomes the fixed containing block, so it clips');
panel.style.transform = 'none';
flush();
assert(document.elementFromPoint(375, 117) === flyout,
       'and removing the transform hands the escape back');

// --- position:absolute follows the same rule ------------------------------
// The panel is positioned, so it IS the containing block: no escape.
flyout.style.position = 'absolute';
flyout.style.left = '200px';
flyout.style.top = '0px';
flush();
const abs = flyout.getBoundingClientRect();
assert(document.elementFromPoint(abs.left + 75, abs.top + 17) !== flyout,
       'an abspos inside a positioned clipper is clipped by it');

// (The mirror case — an abspos escaping a *static* clipper, whose box is not
// its containing block — is covered as a unit test in htmlayout's
// testHitFixedEscapesScrollingAncestor, where the ancestor chain can be built
// without a document's other content in the way.)

root.innerHTML = '';
console.log('PASS: out-of-flow boxes escape the overflow of ancestors below their containing block');
