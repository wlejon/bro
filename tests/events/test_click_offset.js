// Test click event offsetX and offsetY computation
const root = document.getElementById('root');

// Positioned element with offset from viewport
root.innerHTML = '<div id="box" style="position:absolute;left:40px;top:60px;width:200px;height:100px;">Click Target</div>';
flush();

let clickEvt = null;
let mousedownEvt = null;
let mouseupEvt = null;
const box = document.getElementById('box');

box.addEventListener('mousedown', (e) => { mousedownEvt = e; });
box.addEventListener('mouseup', (e) => { mouseupEvt = e; });
box.addEventListener('click', (e) => { clickEvt = e; });

// Click at viewport coordinate (90, 110) -> relative to box (40, 60), offsetX = 50, offsetY = 50
click(90, 110);

assert(clickEvt !== null, 'click event received');
assert(mousedownEvt !== null, 'mousedown event received');
assert(mouseupEvt !== null, 'mouseup event received');

// Check client coords
assert(clickEvt.clientX === 90, 'click clientX is 90');
assert(clickEvt.clientY === 110, 'click clientY is 110');

// Check offsets: should match mousedown/mouseup offsets
assert(Math.abs(clickEvt.offsetX - 50) < 1, 'click offsetX should be ~50 (got ' + clickEvt.offsetX + ')');
assert(Math.abs(clickEvt.offsetY - 50) < 1, 'click offsetY should be ~50 (got ' + clickEvt.offsetY + ')');
assert(clickEvt.offsetX === mousedownEvt.offsetX, 'click offsetX matches mousedown offsetX');
assert(clickEvt.offsetY === mousedownEvt.offsetY, 'click offsetY matches mousedown offsetY');
assert(clickEvt.offsetX === mouseupEvt.offsetX, 'click offsetX matches mouseup offsetX');
assert(clickEvt.offsetY === mouseupEvt.offsetY, 'click offsetY matches mouseup offsetY');

// Cleanup
root.innerHTML = '';
flush();
