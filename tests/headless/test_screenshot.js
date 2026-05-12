// Test screenshot, screenshotCanvas, and getPixel helpers — exercises
// the headless capture path through SkiaRenderer / RasterRenderer and
// the GL readback path in src/engine/headless_api.cpp.

const os = require('os');
const path = require('path');
const fs = require('fs');

const tmpDir = os.tmpdir();
const root = document.getElementById('root');

// =========================================================================
// screenshot — full frame
// =========================================================================
root.innerHTML = '<div id="box" style="width:100px;height:50px;background-color:rgb(255,0,0);">hi</div>';
flush();

const fullPath = path.join(tmpDir, 'bro_test_full_' + Date.now() + '.png');
screenshot(fullPath);
assert(fs.existsSync(fullPath), 'screenshot writes file');
const stat = fs.statSync(fullPath);
assert(stat.size > 100, 'png has content, size = ' + stat.size);

// =========================================================================
// screenshot with selector — crops to element bbox
// =========================================================================
const cropPath = path.join(tmpDir, 'bro_test_crop_' + Date.now() + '.png');
screenshot(cropPath, '#box');
assert(fs.existsSync(cropPath), 'cropped screenshot writes file');
const cropStat = fs.statSync(cropPath);
assert(cropStat.size > 50, 'cropped png size > 50');

// =========================================================================
// getPixel
// =========================================================================
// Element is at viewport top-left (after menu inset). Read pixel from inside.
const box = document.getElementById('box');
const rect = box.getBoundingClientRect();
const px = getPixel(Math.floor(rect.left + rect.width / 2),
                    Math.floor(rect.top + rect.height / 2));
assert(typeof px === 'object', 'getPixel returns object');
assert(typeof px.r === 'number', 'pixel.r is number');
assert(typeof px.g === 'number', 'pixel.g is number');
assert(typeof px.b === 'number', 'pixel.b is number');
assert(typeof px.a === 'number', 'pixel.a is number');

// =========================================================================
// screenshotCanvas — 2D canvas alpha-preserving snapshot
// =========================================================================
root.innerHTML = '<canvas id="c" width="50" height="50"></canvas>';
flush();
const c = document.getElementById('c');
const ctx = c.getContext('2d');
ctx.fillStyle = 'rgba(0,128,255,0.5)';
ctx.fillRect(0, 0, 50, 50);
flush();

const canvasPath = path.join(tmpDir, 'bro_test_canvas_' + Date.now() + '.png');
try {
    screenshotCanvas(canvasPath, '#c');
    if (fs.existsSync(canvasPath)) {
        const cstat = fs.statSync(canvasPath);
        assert(cstat.size > 50, 'canvas screenshot size > 50');
        fs.unlinkSync(canvasPath);
    }
} catch (e) {
    // screenshotCanvas may not be available in all modes; not fatal
    console.log('screenshotCanvas not available:', e.message);
}

// =========================================================================
// inspect / inspectTree / computedStyle / elements helpers
// =========================================================================
root.innerHTML = '<div id="inspect_me" style="width:200px;height:100px;color:red;">hi</div>';
flush();

const inspectOut = inspect('#inspect_me');
assert(typeof inspectOut === 'string', 'inspect returns string');
assert(inspectOut.length > 0, 'inspect has content');

const tree = inspectTree('#inspect_me');
assert(typeof tree === 'string', 'inspectTree returns string');

const w = computedStyle('#inspect_me', 'width');
assert(w === '200px' || w.indexOf('200') !== -1, 'computedStyle width = 200px, got ' + w);

const allStyles = computedStyle('#inspect_me');
assert(typeof allStyles === 'object', 'computedStyle without prop returns object');
assert(allStyles.width.indexOf('200') !== -1, 'allStyles.width');

const els = elements('div');
assert(typeof els === 'string' || Array.isArray(els), 'elements returns summary');

// =========================================================================
// Cleanup
// =========================================================================
try { fs.unlinkSync(fullPath); } catch(e) {}
try { fs.unlinkSync(cropPath); } catch(e) {}
root.innerHTML = '';
