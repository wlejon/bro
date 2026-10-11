// A box sized to its content (absolutely positioned, inline-block, float)
// whose child has `width: 100%`: the percentage depends on the very size
// being measured, so for that measurement it counts as auto (CSS Sizing 3
// §5.2.1) and the child's content is what sizes the box. The child then
// fills it. Kit menus are built this way (a row per item, `width: 100%`,
// label and shortcut in a flex row); without this every menu stayed at its
// min-width and cut its labels.

const root = document.getElementById('root') || document.body;
const style = document.createElement('style');
document.head.appendChild(style);
style.textContent = `
  body { margin: 0; font: 14px sans-serif; }
  .menu { position: absolute; left: 10px; top: 10px; min-width: 60px; padding: 4px; }
  .item { display: flex; width: 100%; gap: 20px; white-space: nowrap; }
  .inl { display: inline-block; }
  .flt { float: left; }
  .wide { width: 100%; white-space: nowrap; }
`;
root.innerHTML =
  '<div class="menu" id="m"><div class="item" id="mi"><span id="lbl">Play Next In The Queue</span><span id="key">Ctrl+Shift+N</span></div></div>' +
  '<div style="position:absolute; top:80px; left:10px"><div class="inl" id="ib"><div class="wide" id="ibc">An inline-block holding a long line</div></div></div>' +
  '<div style="position:absolute; top:140px; left:10px"><div class="flt" id="fl"><div class="wide" id="flc">A float holding a long line</div></div></div>';
flush();

const w = (id) => document.getElementById(id).getBoundingClientRect().width;
const contentW = w('lbl') + 20 + w('key');
assert(w('m') >= contentW, `the menu is as wide as its row's content: ${w('m')} >= ${contentW}`);
assert(Math.abs(w('mi') - (w('m') - 8)) <= 1, `the row fills the menu: ${w('mi')} of ${w('m')}`);
assert(w('ib') > 150, `an inline-block takes its width:100% child's content: ${w('ib')}`);
assert(Math.abs(w('ibc') - w('ib')) <= 1, 'and the child fills it');
assert(w('fl') > 120, `a float takes its width:100% child's content: ${w('fl')}`);

