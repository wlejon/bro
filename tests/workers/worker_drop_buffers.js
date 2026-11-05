// Makes a big buffer per message and drops it, answering synchronously (no
// promise, so no microtask queued): the Image Viewer's decode worker.
self.onmessage = (e) => {
  const pixels = new Uint8Array(e.data.bytes);
  pixels[pixels.length - 1] = 1;
  self.postMessage(pixels[pixels.length - 1]);
};
