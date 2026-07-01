// Test getBoundingClientRect() with a CSS transform on the element itself —
// the classic "position:absolute; left:50%; transform:translateX(-50%)"
// centering trick (e.g. broworkshop's scene-editor toolbar). The transform
// must shift the reported position, not just be ignored.

const root = document.getElementById('root');
root.innerHTML = '<div id="container" style="position:absolute;left:0;top:0;width:400px;height:100px;">' +
                  '<div id="box" style="position:absolute;left:50%;top:10px;width:150px;height:80px;transform:translateX(-50%);">box</div>' +
                  '</div>';
flush();

const box = document.getElementById('box');
const rect = box.getBoundingClientRect();

// Size is unaffected by a pure translate.
assert(Math.abs(rect.width - 150) < 1, 'width is ~150, got ' + rect.width);
assert(Math.abs(rect.height - 80) < 1, 'height is ~80, got ' + rect.height);

// left:50% of the 400px container resolves to 200px pre-transform, then
// translateX(-50%) shifts left by 50% of the box's OWN width (150px) = 75px.
assert(Math.abs(rect.left - 125) < 1, 'translateX(-50%) centering applied, got ' + rect.left);
assert(Math.abs(rect.top - 10) < 1, 'top is unaffected by the X-only transform, got ' + rect.top);

// Cleanup
root.innerHTML = '';
