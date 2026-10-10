// The worker behind test_pipeline_memory.js: a picture per request, made here
// and transferred to the page as an ImageBitmap, as an image viewer's decoder
// thread does.
self.onmessage = async (e) => {
    const { w, h, v } = e.data;
    const px = new Uint8ClampedArray(w * h * 4);
    px.fill(v);
    const bitmap = await createImageBitmap(new ImageData(px, w, h));
    self.postMessage({ bitmap }, [bitmap]);
};
