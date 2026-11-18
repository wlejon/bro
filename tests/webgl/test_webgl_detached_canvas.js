// A WebGL context on a canvas outside the document (a capability probe's
// createElement('canvas')) starts at the web's 300x150, not the viewport:
// xeokit reads the probe canvas's size back.
const c = document.createElement('canvas');
const gl = c.getContext('webgl', { antialias: true });
if (!gl) {
    missingGpuContext('webgl');
} else {
    assert(c.width === 300 && c.height === 150, 'canvas size ' + c.width + 'x' + c.height);
    assert(gl.drawingBufferWidth === 300 && gl.drawingBufferHeight === 150,
           'drawing buffer ' + gl.drawingBufferWidth + 'x' + gl.drawingBufferHeight);
    console.log('test_webgl_detached_canvas: OK');
}
