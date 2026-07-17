// ::placeholder and ::selection — styled-box pseudo-elements. The cascade
// resolves them per element (htmlayout resolvePseudo); the controls consult
// the resolved style for the small subset that applies (::placeholder:
// color/opacity/font-*; ::selection: color/background-color) and fall back to
// the legacy hardcoded paint when no rule targets them.
//
// Pixel-based: placeholder/value text uses U+2588 FULL BLOCK glyphs so an
// interior pixel is solidly the text color (no anti-aliasing guesswork).

const root = document.getElementById('root');
const BLOCKS = '██████';

function centerOf(el) {
  const r = el.getBoundingClientRect();
  return [Math.floor(r.left + 20), Math.floor(r.top + r.height / 2)];
}

// =========================================================================
// ::placeholder color on <input>
// =========================================================================
const style = document.createElement('style');
style.textContent = `
  input, textarea {
    font-size: 28px; width: 240px; height: 40px;
    padding: 0; border: 0; background-color: rgb(255,255,255);
  }
  input.ph::placeholder { color: rgb(200, 30, 30); }
  textarea.ph::placeholder { color: rgb(30, 30, 200); }
  input.sel::selection { background-color: rgb(255, 215, 0); }
  input.selc::selection { background-color: rgb(255, 215, 0); color: rgb(0, 160, 0); }
  div.docsel::selection { background-color: rgb(0, 200, 0); }
`;
document.head.appendChild(style);

root.innerHTML =
  `<input id="in-plain" placeholder="${BLOCKS}">` +
  `<input id="in-ph" class="ph" placeholder="${BLOCKS}">` +
  `<input id="in-sel" class="sel" value="      ">` +
  `<input id="in-selc" class="selc" value="${BLOCKS}">` +
  `<textarea id="ta-ph" class="ph" placeholder="${BLOCKS}"></textarea>`;
flush();

// Unstyled input keeps the legacy muted-gray placeholder (default unchanged).
{
  const [x, y] = centerOf(document.getElementById('in-plain'));
  const px = getPixel(x, y);
  assert(Math.abs(px.r - px.g) < 25 && Math.abs(px.g - px.b) < 25,
         `unstyled placeholder stays gray, got rgb(${px.r},${px.g},${px.b})`);
  assert(px.r > 100 && px.r < 240,
         `unstyled placeholder is muted (not black/white), got r=${px.r}`);
}

// input.ph::placeholder { color: rgb(200,30,30) } → red placeholder glyphs.
{
  const [x, y] = centerOf(document.getElementById('in-ph'));
  const px = getPixel(x, y);
  assert(px.r > 150 && px.g < 90 && px.b < 90,
         `styled input placeholder is red, got rgb(${px.r},${px.g},${px.b})`);
}

// textarea.ph::placeholder { color: rgb(30,30,200) } → blue placeholder glyphs.
{
  const ta = document.getElementById('ta-ph');
  const r = ta.getBoundingClientRect();
  // Textarea draws its first line at the content top, not vertically centered.
  const px = getPixel(Math.floor(r.left + 20), Math.floor(r.top + 14));
  assert(px.b > 150 && px.r < 90 && px.g < 90,
         `styled textarea placeholder is blue, got rgb(${px.r},${px.g},${px.b})`);
}

// =========================================================================
// ::selection background-color on <input> (value is spaces → wash visible)
// =========================================================================
// Controls take visual focus via the pointer path (same as the other input
// tests) — element.focus() alone doesn't reach the control's focused_ flag.
function focusControl(el) {
  const r = el.getBoundingClientRect();
  click(r.left + 5, r.top + r.height / 2);
}

{
  const inp = document.getElementById('in-sel');
  focusControl(inp);
  inp.setSelectionRange(0, 6);
  flush();
  const [x, y] = centerOf(inp);
  const px = getPixel(x, y);
  assert(px.r > 200 && px.g > 150 && px.b < 100,
         `input ::selection wash is gold, got rgb(${px.r},${px.g},${px.b})`);
}

// ::selection color: selected block glyphs repaint in the ::selection color.
{
  const inp = document.getElementById('in-selc');
  focusControl(inp);
  inp.setSelectionRange(0, 6);
  flush();
  const [x, y] = centerOf(inp);
  const px = getPixel(x, y);
  assert(px.g > 110 && px.r < 100 && px.b < 100,
         `input ::selection color repaints glyphs green, got rgb(${px.r},${px.g},${px.b})`);
}

// Unstyled input selection keeps the legacy accent wash (default unchanged).
{
  const inp = document.getElementById('in-plain');
  inp.setAttribute('value', '      ');
  flush();
  focusControl(inp);
  inp.setSelectionRange(0, 6);
  flush();
  const [x, y] = centerOf(inp);
  const px = getPixel(x, y);
  // Accent wash (#0078d7 at 0.35 over white) reads blue-dominant.
  assert(px.b > px.r && px.b > 150,
         `unstyled selection wash stays accent blue, got rgb(${px.r},${px.g},${px.b})`);
}

// =========================================================================
// Document text ::selection (Engine::drawSelectionHighlight)
// =========================================================================
root.innerHTML =
  '<div id="doc-styled" class="docsel" style="font-size:28px">' + BLOCKS + '</div>' +
  '<div id="doc-plain" style="font-size:28px">' + BLOCKS + '</div>';
flush();

function selectDivText(div) {
  const sel = document.getSelection();
  sel.removeAllRanges();
  const range = document.createRange();
  const tn = div.firstChild;
  range.setStart(tn, 0);
  range.setEnd(tn, div.textContent.length);
  sel.addRange(range);
}

// Styled: div.docsel::selection { background-color: rgb(0,200,0) } — the
// overlay (alpha-capped to stay legible) tints the black glyphs green.
{
  const div = document.getElementById('doc-styled');
  selectDivText(div);
  flush();
  const [x, y] = centerOf(div);
  const px = getPixel(x, y);
  assert(px.g > px.r + 30 && px.g > px.b + 30 && px.g > 60,
         `document ::selection highlight is green, got rgb(${px.r},${px.g},${px.b})`);
}

// Unstyled: the default highlight is translucent accent blue over the glyphs —
// NOT opaque white (regression: the old int-literal bromath::Color saturated
// to opaque white and blanked the selected text).
{
  const div = document.getElementById('doc-plain');
  selectDivText(div);
  flush();
  const [x, y] = centerOf(div);
  const px = getPixel(x, y);
  assert(!(px.r > 240 && px.g > 240 && px.b > 240),
         `default doc selection must not be opaque white, got rgb(${px.r},${px.g},${px.b})`);
  assert(px.b > px.r,
         `default doc selection reads blue over black glyphs, got rgb(${px.r},${px.g},${px.b})`);
}

// Cleanup
document.getSelection().removeAllRanges();
root.innerHTML = '';

console.log('PASS ::placeholder / ::selection');
