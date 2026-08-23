// Test createImageBitmap canvas 2D source, drawing, cropping, and error handling.
// Exercises src/js/imagebitmap_bindings.cpp and CanvasRenderingContext2D::drawImage

assert(typeof createImageBitmap === 'function', 'createImageBitmap global exists');

// Helper to create solid color canvas
function createTestCanvas(w, h, color) {
    const c = document.createElement('canvas');
    c.setAttribute('width', String(w));
    c.setAttribute('height', String(h));
    document.body.appendChild(c);
    flush();
    const ctx = c.getContext('2d');
    ctx.fillStyle = color;
    ctx.fillRect(0, 0, w, h);
    return { canvas: c, ctx };
}

// 1. Create ImageBitmap from HTMLCanvasElement
const src = createTestCanvas(16, 16, '#00ff00');
const bmpFromCanvas = await createImageBitmap(src.canvas);
assert(bmpFromCanvas instanceof ImageBitmap, 'bitmap from canvas is ImageBitmap');
assert(bmpFromCanvas.width === 16 && bmpFromCanvas.height === 16, 'bitmap dimensions match canvas');

// 2. Draw ImageBitmap onto destination canvas
const dst = createTestCanvas(32, 32, '#000000');
dst.ctx.drawImage(bmpFromCanvas, 0, 0);
let pixel = dst.ctx.getImageData(0, 0, 1, 1).data;
assert(pixel[0] === 0 && pixel[1] === 255 && pixel[2] === 0, 'destination received green pixels');

// 3. Cropped createImageBitmap from canvas
const croppedBmp = await createImageBitmap(src.canvas, 4, 4, 8, 8);
assert(croppedBmp.width === 8 && croppedBmp.height === 8, 'cropped bitmap dimensions are 8x8');
dst.ctx.clearRect(0, 0, 32, 32);
dst.ctx.drawImage(croppedBmp, 0, 0, 16, 16);
pixel = dst.ctx.getImageData(0, 0, 1, 1).data;
assert(pixel[0] === 0 && pixel[1] === 255 && pixel[2] === 0, 'scaled draw of cropped bitmap matches');

// 4. createImageBitmap from ImageData
const imgData = dst.ctx.getImageData(0, 0, 8, 8);
const bmpFromData = await createImageBitmap(imgData);
assert(bmpFromData.width === 8 && bmpFromData.height === 8, 'bitmap from ImageData matches size');

// 5. createImageBitmap from another ImageBitmap
const bmpFromBmp = await createImageBitmap(bmpFromCanvas);
assert(bmpFromBmp.width === 16 && bmpFromBmp.height === 16, 'bitmap from ImageBitmap matches size');

// 6. Error handling: missing argument or invalid source
let threwNoArg = false;
try {
    await createImageBitmap();
} catch (e) {
    threwNoArg = true;
}
assert(threwNoArg, 'createImageBitmap() with no args throws/rejects');

let threwInvalid = false;
try {
    await createImageBitmap(null);
} catch (e) {
    threwInvalid = true;
}
assert(threwInvalid, 'createImageBitmap(null) throws/rejects');

// Cleanup
bmpFromCanvas.close();
croppedBmp.close();
bmpFromData.close();
bmpFromBmp.close();
document.body.removeChild(src.canvas);
document.body.removeChild(dst.canvas);

console.log('test_create_image_bitmap: passed');
