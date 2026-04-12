// Test Canvas 2D API — path ops, transforms, drawing, text measurement.
// Exercises the canvas_bindings.cpp surface via headless --no-gpu.

// --- Setup: create a <canvas> in the DOM and get the 2d context ---
var canvas = document.createElement('canvas');
canvas.setAttribute('width', '200');
canvas.setAttribute('height', '200');
document.body.appendChild(canvas);
flush();

var ctx = canvas.getContext('2d');
assert(ctx !== null && ctx !== undefined, 'getContext("2d") returns a context');

// --- fillStyle / strokeStyle ---
ctx.fillStyle = 'red';
assert(ctx.fillStyle.indexOf('255') !== -1, 'fillStyle accepts named color, got: ' + ctx.fillStyle);
ctx.strokeStyle = '#00ff00';
assert(ctx.strokeStyle.indexOf('255') !== -1, 'strokeStyle accepts hex color, got: ' + ctx.strokeStyle);

// --- lineWidth ---
ctx.lineWidth = 3;
assert(ctx.lineWidth === 3, 'lineWidth getter returns set value');

// --- lineCap / lineJoin ---
ctx.lineCap = 'round';
assert(ctx.lineCap === 'round', 'lineCap round');
ctx.lineJoin = 'bevel';
assert(ctx.lineJoin === 'bevel', 'lineJoin bevel');

// --- globalAlpha ---
ctx.globalAlpha = 0.5;
assert(Math.abs(ctx.globalAlpha - 0.5) < 0.01, 'globalAlpha getter');

// --- globalCompositeOperation ---
ctx.globalCompositeOperation = 'source-over';
assert(ctx.globalCompositeOperation === 'source-over', 'globalCompositeOperation');

// --- textAlign / textBaseline ---
ctx.textAlign = 'center';
assert(ctx.textAlign === 'center', 'textAlign center');
ctx.textBaseline = 'middle';
assert(ctx.textBaseline === 'middle', 'textBaseline middle');

// --- save / restore ---
ctx.fillStyle = '#0000ff';
ctx.save();
ctx.fillStyle = '#ff0000';
// fillStyle returns rgba format
assert(ctx.fillStyle.indexOf('255,0,0') !== -1, 'fillStyle changed after save');
ctx.restore();
assert(ctx.fillStyle.indexOf('0,0,255') !== -1, 'fillStyle restored after restore');

// --- Basic drawing methods (should not throw) ---
ctx.fillRect(0, 0, 100, 100);
ctx.strokeRect(10, 10, 80, 80);
ctx.clearRect(20, 20, 60, 60);

// --- Path methods ---
ctx.beginPath();
ctx.moveTo(0, 0);
ctx.lineTo(100, 100);
ctx.lineTo(100, 0);
ctx.closePath();
ctx.stroke();
ctx.fill();

// --- Arc ---
ctx.beginPath();
ctx.arc(50, 50, 25, 0, Math.PI * 2);
ctx.stroke();

// --- arcTo ---
ctx.beginPath();
ctx.moveTo(0, 0);
ctx.arcTo(50, 0, 50, 50, 20);
ctx.stroke();

// --- bezierCurveTo ---
ctx.beginPath();
ctx.moveTo(0, 0);
ctx.bezierCurveTo(10, 50, 90, 50, 100, 0);
ctx.stroke();

// --- quadraticCurveTo ---
ctx.beginPath();
ctx.moveTo(0, 100);
ctx.quadraticCurveTo(50, 0, 100, 100);
ctx.stroke();

// --- rect (path, not fill/stroke) ---
ctx.beginPath();
ctx.rect(5, 5, 30, 30);
ctx.fill();

// --- ellipse ---
ctx.beginPath();
ctx.ellipse(50, 50, 40, 20, 0, 0, Math.PI * 2);
ctx.stroke();

// --- isPointInPath ---
ctx.beginPath();
ctx.rect(0, 0, 100, 100);
assert(ctx.isPointInPath(50, 50) === true, 'isPointInPath inside rect');
assert(ctx.isPointInPath(150, 150) === false, 'isPointInPath outside rect');

// --- Transforms ---
ctx.resetTransform();
ctx.translate(10, 20);
ctx.scale(2, 2);
ctx.rotate(0.1);
ctx.setTransform(1, 0, 0, 1, 0, 0); // identity
ctx.transform(1, 0, 0, 1, 5, 5);     // translate by (5,5)
ctx.resetTransform();

// --- fillText / strokeText (should not throw) ---
ctx.fillText('hello', 10, 50);
ctx.strokeText('world', 10, 70);

// --- measureText ---
ctx.font = '16px sans-serif';
var metrics = ctx.measureText('test');
assert(metrics !== null && metrics !== undefined, 'measureText returns object');
assert(typeof metrics.width === 'number', 'measureText has width');
assert(metrics.width >= 0, 'measureText width is non-negative');
assert(typeof metrics.actualBoundingBoxAscent === 'number', 'measureText has ascent');
assert(typeof metrics.actualBoundingBoxDescent === 'number', 'measureText has descent');

// --- clip ---
ctx.beginPath();
ctx.rect(10, 10, 50, 50);
ctx.clip();
ctx.fillRect(0, 0, 200, 200); // should be clipped

// --- reset ---
ctx.reset();

// --- getImageData / putImageData / createImageData ---
var imgData = ctx.createImageData(10, 10);
assert(imgData !== null, 'createImageData returns object');
assert(imgData.width === 10, 'createImageData width');
assert(imgData.height === 10, 'createImageData height');
assert(imgData.data.length === 400, 'createImageData data length = w*h*4');

// putImageData should not throw
ctx.putImageData(imgData, 0, 0);

// getImageData
var got = ctx.getImageData(0, 0, 5, 5);
assert(got !== null, 'getImageData returns object');
assert(got.width === 5, 'getImageData width');
assert(got.height === 5, 'getImageData height');
assert(got.data.length === 100, 'getImageData data length');

// --- shadowColor ---
ctx.shadowColor = 'rgba(0,0,0,0.5)';
// Just verify it doesn't throw; getter format may vary

// --- polyline (bro extension) ---
ctx.beginPath();
ctx.polyline([0, 0, 50, 50, 100, 0]);
ctx.stroke();

// --- Cleanup ---
document.body.removeChild(canvas);
