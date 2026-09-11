// Worker for test_imagebitmap.js — receives a transferred/cloned ImageBitmap
// and reports its dimensions back, exercising the zero-copy Worker transfer
// path in message serializer (kTransferImageBitmap).

self.onmessage = (e) => {
    const data = e.data;
    if (data && data.cmd === 'dims') {
        const bmp = data.bitmap;
        self.postMessage({ width: bmp.width, height: bmp.height });
    } else {
        self.postMessage({ unknown: true });
    }
};
