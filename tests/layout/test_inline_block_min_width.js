// min-width and max-width clamp an auto-width inline-block (CSS2 §10.4).

const root = document.getElementById('root');
root.innerHTML =
    '<div><span id="kv" style="display:inline-block;min-width:96px">lobby id</span><span id="after">X</span></div>' +
    '<div><span id="cap" style="display:inline-block;max-width:40px">a long label</span></div>';
flush();

const kv = document.getElementById('kv').getBoundingClientRect();
const after = document.getElementById('after').getBoundingClientRect();
assert(Math.abs(kv.width - 96) < 0.5, 'min-width widens the inline-block, got ' + kv.width);
assert(Math.abs(after.left - 96) < 0.5, 'the next inline starts after it, got ' + after.left);

const cap = document.getElementById('cap').getBoundingClientRect();
assert(Math.abs(cap.width - 40) < 0.5, 'max-width caps the inline-block, got ' + cap.width);

root.innerHTML = '';
