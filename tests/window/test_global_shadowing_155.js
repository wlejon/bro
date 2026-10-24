// Issue #155: Top-level function/var declarations in classic <script> blocks
// shadowing replaceable window.* host globals (blur, focus, close, open, scroll,
// scrollTo, scrollBy, etc.) across scripts, -e, bro-ctl eval, and iframe realms,
// while preserving window.name's string-coercing accessor.

const cp = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

// 1. Same-<script> top-level `function` (including pre-declaration hoisting)
// and `var` shadowing of replaceable window.* host globals + `window.name` coercion:
assert(blur() === 'blur-ok', 'hoisted function blur() works before its declaration');
assert(window.blur() === 'blur-ok', 'hoisted window.blur() works before its declaration');

var __shadowLog = [];
function blur() {
  if (__shadowLog) __shadowLog.push('custom-blur');
  return 'blur-ok';
}
var focus = function() {
  __shadowLog.push('custom-focus');
  return 'focus-ok';
};
var scrollBy = function(dx, dy) {
  __shadowLog.push('custom-scrollBy:' + dx + ',' + dy);
  return dx + dy;
};
var name = 42;

assert(blur() === 'blur-ok' && window.blur() === 'blur-ok',
       'same <script> bare blur() and window.blur() hit top-level function');
assert(focus() === 'focus-ok' && window.focus() === 'focus-ok',
       'same <script> bare focus() and window.focus() hit top-level var');
assert(scrollBy(3, 4) === 7 && window.scrollBy(5, 6) === 11,
       'same <script> bare scrollBy() and window.scrollBy() hit top-level var');
assert(typeof name === 'string' && name === '42',
       'var name = 42 coerces through window.name setter to string "42", got: ' + (typeof name) + ':' + name);
assert(typeof window.name === 'string' && window.name === '42',
       'window.name is string "42", got: ' + (typeof window.name) + ':' + window.name);

// 2. `controlCommand('eval', ...)` (the handler backing `bro-ctl eval`):
const ctlId = controlCommand('eval', 'blur() + "|" + window.blur() + "|" + focus() + "|" + window.focus() + "|" + scrollBy(7, 8) + "|" + name + "|" + typeof window.name');
const ctlRes = controlResult(ctlId);
assert(ctlRes && ctlRes.ok === true, 'controlCommand eval succeeded: ' + JSON.stringify(ctlRes));
assert(ctlRes.payload === 'blur-ok|blur-ok|focus-ok|focus-ok|15|42|string',
       'bro-ctl eval sees shadowed globals: ' + JSON.stringify(ctlRes));

// 3. <iframe> realm isolation + multi-<script> classic script shadowing inside the iframe:
// - Script 1 in the iframe checks that the iframe starts with its own un-shadowed
//   native `blur()` (returns undefined) and empty `window.name` (""), NOT the parent's overrides.
// - Script 2 in the iframe declares `function blur()`, `var focus = ...`, and `var name = 99`,
//   testing hoisting before declaration and same-script calls.
// - Script 3 in the iframe (a later <script> in the same HTML document) calls bare
//   `blur()`, `window.blur()`, `focus()`, `window.focus()`, and reads `name` / `window.name`.
const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'bro_shadow_155_'));
const childDir = path.join(tmpDir, 'child_iframe');
fs.mkdirSync(childDir, { recursive: true });
fs.writeFileSync(path.join(childDir, 'index.html'), `<!DOCTYPE html>
<html><body>
<script>
  window.__childInitialName = window.name;
  window.__childInitialBlurResult = String(blur());
</script>
<script>
  window.__childHoistedBlur = blur() + ':' + window.blur();
  function blur() { return 'iframe-blur'; }
  var focus = function() { return 'iframe-focus'; };
  var name = 99;
  window.__childSameScript = [blur(), window.blur(), focus(), window.focus(), typeof name, name, window.name].join('|');
</script>
<script>
  window.__childLaterScript = [blur(), window.blur(), focus(), window.focus(), typeof name, name, window.name].join('|');
</script>
</body></html>`);

const ifr = document.createElement('iframe');
ifr.setAttribute('src', childDir);
ifr.style.cssText = 'width: 200px; height: 100px';
document.body.appendChild(ifr);
flush();

const cw = ifr.contentWindow;
assert(cw !== null && cw !== window, 'iframe has its own contentWindow');
assert(cw.__childInitialName === '',
       'iframe starts with empty window.name, got: ' + cw.__childInitialName);
assert(cw.__childInitialBlurResult === 'undefined',
       'iframe initial blur() is native no-op (undefined), not parent override: ' + cw.__childInitialBlurResult);
assert(cw.__childHoistedBlur === 'iframe-blur:iframe-blur',
       'iframe script 2 hoists function blur() before its declaration: ' + cw.__childHoistedBlur);
assert(cw.__childSameScript === 'iframe-blur|iframe-blur|iframe-focus|iframe-focus|string|99|99',
       'iframe script 2 shadows blur, focus, and coerces var name = 99: ' + cw.__childSameScript);
assert(cw.__childLaterScript === 'iframe-blur|iframe-blur|iframe-focus|iframe-focus|string|99|99',
       'iframe script 3 (later <script>) sees script 2 overrides: ' + cw.__childLaterScript);

// Verify parent realm was NOT clobbered by iframe's declarations:
assert(blur() === 'blur-ok' && window.blur() === 'blur-ok',
       'parent blur() unaffected by iframe blur()');
assert(focus() === 'focus-ok' && window.focus() === 'focus-ok',
       'parent focus() unaffected by iframe focus()');
assert(name === '42' && window.name === '42',
       'parent window.name unaffected by iframe var name = 99');

// 4. End-to-end multi-<script> HTML page with `-e` and `controlCommand('eval', ...)` via bro-headless:
const appDir = path.join(tmpDir, 'app');
fs.mkdirSync(appDir, { recursive: true });
fs.writeFileSync(path.join(appDir, 'bro.json'), JSON.stringify({
  name: 'shadow-test-155',
  entry: 'index.html'
}));
fs.writeFileSync(path.join(appDir, 'index.html'), `<!DOCTYPE html>
<html><body>
<script>
  window.__pageHoisted = blur() + ':' + window.blur();
  function blur() { return 'page-blur'; }
  var close = function() { return 'page-close'; };
  var name = 123;
  window.__pageFirstScript = [blur(), window.blur(), close(), window.close(), typeof name, name, window.name].join('|');
</script>
<script>
  window.__pageSecondScript = [blur(), window.blur(), close(), window.close(), typeof name, name, window.name].join('|');
</script>
</body></html>`);

const exeName = process.platform === 'win32' ? 'bro-headless.exe' : 'bro-headless';
const exe = path.join(process.env.BRO_EXE_DIR || '/home/j/projects/bro/build', exeName);
const child = cp.spawnSync(exe, [
  appDir,
  '-e',
  'const ctl = controlResult(controlCommand("eval", "blur() + \\":\\" + close() + \\":\\" + name")); ' +
  'console.log("E_OUT:" + window.__pageHoisted + "||" + window.__pageFirstScript + "||" + window.__pageSecondScript + "||" + ' +
  '[blur(), window.blur(), close(), window.close(), typeof name, name, window.name, ctl.payload].join("|")); bro.quit();'
], { encoding: 'utf8', timeout: 30000 });

const childOut = (child.stdout || '') + (child.stderr || '');
assert(child.status === 0, 'child bro-headless exited 0:\n' + childOut);
const expectedLine =
  'E_OUT:page-blur:page-blur||' +
  'page-blur|page-blur|page-close|page-close|string|123|123||' +
  'page-blur|page-blur|page-close|page-close|string|123|123||' +
  'page-blur|page-blur|page-close|page-close|string|123|123|page-blur:page-close:123';
assert(childOut.includes(expectedLine),
       'page <script> 1, <script> 2, -e, and bro-ctl eval all see top-level function/var overrides:\n' + childOut);

fs.rmSync(tmpDir, { recursive: true, force: true });
console.log('test_global_shadowing_155 OK');
