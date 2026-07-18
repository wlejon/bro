// CSS `cursor` → OS cursor shape mapping, observed through the headless
// currentCursor() seam. The engine re-resolves the hovered element's computed
// cursor on every mouseMove; in headless the mapping runs (resolvedCursor_)
// while the SDL cursor apply is windowed-only, so this asserts the full
// hit-test → computed-style → shape pipeline short of the actual OS call.
// The windowed SDL_SetCursor half is verified manually.

const root = document.getElementById('root');
root.innerHTML = `
  <div id="ptr"   style="position:absolute;left:0;top:0;width:100px;height:40px;cursor:pointer"></div>
  <div id="txt"   style="position:absolute;left:0;top:40px;width:100px;height:40px;cursor:text"></div>
  <div id="grab"  style="position:absolute;left:0;top:80px;width:100px;height:40px;cursor:grab"></div>
  <div id="plain" style="position:absolute;left:0;top:120px;width:100px;height:40px"></div>
  <div id="hide"  style="position:absolute;left:0;top:160px;width:100px;height:40px;cursor:none"></div>
  <div id="inh"   style="position:absolute;left:0;top:200px;width:100px;height:40px;cursor:crosshair">
    <span id="inhchild" style="display:block;width:100%;height:100%"></span>
  </div>
  <div id="col"   style="position:absolute;left:0;top:240px;width:100px;height:40px;cursor:col-resize"></div>
  <div id="nwse"  style="position:absolute;left:0;top:280px;width:100px;height:40px;cursor:se-resize"></div>
  <div id="na"    style="position:absolute;left:0;top:320px;width:100px;height:40px;cursor:not-allowed"></div>
  <div id="fall"  style="position:absolute;left:0;top:360px;width:100px;height:40px;cursor:url(missing.png), wait"></div>
  <div id="bogus" style="position:absolute;left:0;top:400px;width:100px;height:40px;cursor:sparkle-wand"></div>
  <input id="tin"  style="position:absolute;left:0;top:440px;width:100px;height:24px">
  <button id="btn" style="position:absolute;left:0;top:480px;width:100px;height:30px">b</button>
  <input id="chk" type="checkbox" style="position:absolute;left:0;top:520px">
  <div id="edit" contenteditable style="position:absolute;left:0;top:560px;width:100px;height:30px">e</div>
`;
flush();

function at(y, expected, label) {
    mouseMove(50, y);
    assert(currentCursor() === expected,
           label + ': expected "' + expected + '", got "' + currentCursor() + '"');
}

// Author-styled keywords
at(20,  'pointer',     'cursor:pointer');
at(60,  'text',        'cursor:text');
at(100, 'move',        'cursor:grab maps to move (SDL has no grab shape)');
at(140, 'default',     'unstyled element');
at(180, 'none',        'cursor:none resolves to hidden');
at(220, 'crosshair',   'cursor inherits into child span');
at(260, 'ew-resize',   'col-resize maps to the EW resize shape');
at(300, 'nwse-resize', 'se-resize maps to the NWSE resize shape');
at(340, 'not-allowed', 'cursor:not-allowed');
at(380, 'wait',        'url() fallback list uses the last keyword');
at(420, 'default',     'unknown keyword falls back to default');

// UA-stylesheet defaults (default_styles.h)
at(452, 'text',    'text input gets the I-beam from the UA sheet');
at(495, 'pointer', 'button gets pointer from the UA sheet');
mouseMove(6, 526);
assert(currentCursor() === 'default',
       'checkbox stays on the arrow: got "' + currentCursor() + '"');
at(575, 'text',    'contenteditable host gets the I-beam');

// Leaving every element restores the default
mouseMove(500, 20);
assert(currentCursor() === 'default',
       'off-element resolves default: got "' + currentCursor() + '"');

// Cleanup
root.innerHTML = '';
