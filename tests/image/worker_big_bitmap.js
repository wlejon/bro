// The worker behind test_imagebitmap_transfer_big.js: makes a W x H picture
// of four solid quadrants (red / green / blue / yellow, as
// test_imagebitmap.js's makeQuadrantRGBA) and transfers it to the page as an
// ImageBitmap, as an image viewer's decoder thread does. After the post it
// reports what its own handle reads, so the page can check it was detached.
self.onmessage = async (e) => {
    const { w, h, tag } = e.data;
    const px = new Uint8ClampedArray(w * h * 4);
    const u32 = new Uint32Array(px.buffer);
    // Little-endian RGBA as one u32: A<<24 | B<<16 | G<<8 | R.
    const RED = 0xff0000ff, GREEN = 0xff00ff00, BLUE = 0xffff0000, YELLOW = 0xff00ffff;
    const half = w >> 1, mid = h >> 1;
    for (let y = 0; y < h; y++) {
        const row = y * w;
        u32.fill(y < mid ? RED : BLUE, row, row + half);
        u32.fill(y < mid ? GREEN : YELLOW, row + half, row + w);
    }
    const bitmap = await createImageBitmap(new ImageData(px, w, h));
    self.postMessage({ tag, bitmap }, [bitmap]);
    self.postMessage({ tag, after: { width: bitmap.width, height: bitmap.height } });
};
