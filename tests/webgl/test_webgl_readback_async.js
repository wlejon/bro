// Asynchronous readback, the WebGL2 way: readPixels into a PIXEL_PACK
// buffer, fenceSync, poll clientWaitSync with timeout 0 on later frames, then
// getBufferSubData. Once the fence has signalled the read waits for nothing:
// fenceSync copied the buffer's GPU-written bytes out to host memory ahead of
// the fence, so getBufferSubData is a memcpy that does not submit the work
// recorded since. The oracle for "did not submit" is a query recorded after
// the fence: its QUERY_RESULT_AVAILABLE stays false while it sits in the
// unsubmitted command buffer, and a read that waited for the stream would
// have submitted and completed it.
//
// Also: a read whose buffer's last GPU write is complete (no fence) copies on
// its own submission, not the open stream's; partial ranges read only what
// they ask for and stay coherent with later full reads and CPU writes; and
// readPixels of a float target into client memory returns the stored bits
// exactly (the same-format row copy).

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '64');
canvas.setAttribute('height', '64');
document.body.appendChild(canvas);
flush();

const gl = canvas.getContext('webgl2');
if (!gl) {
    missingGpuContext('webgl2');
} else {
    const timer = gl.getExtension('EXT_disjoint_timer_query_webgl2');
    assert(gl.getExtension('EXT_color_buffer_float') !== null, 'EXT_color_buffer_float');

    // A 16x8 RGBA32F target holding distinct floats.
    const W = 16, H = 8;
    const src = new Float32Array(W * H * 4);
    for (let i = 0; i < src.length; i++) src[i] = i * 0.25 - 100.5;
    const tex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA32F, W, H, 0, gl.RGBA, gl.FLOAT, src);
    const fb = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, fb);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, tex, 0);
    assert(gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_COMPLETE, 'RGBA32F target complete');

    const sameAs = (got, want, msg) => {
        let bad = -1;
        for (let i = 0; i < want.length && bad < 0; i++) if (got[i] !== want[i]) bad = i;
        assert(bad < 0, msg + (bad < 0 ? '' : ': first mismatch at ' + bad + ' got ' + got[bad] + ' want ' + want[bad]));
    };

    // --- readPixels RGBA/FLOAT into client memory: exact bits ---------------
    const direct = new Float32Array(W * H * 4);
    gl.readPixels(0, 0, W, H, gl.RGBA, gl.FLOAT, direct);
    assert(gl.getError() === gl.NO_ERROR, 'readPixels RGBA32F to client memory');
    sameAs(direct, src, 'same-format readPixels returns the stored floats');

    // A rectangle hanging off the framebuffer, with a row length: in-bounds
    // texels land where the PACK state puts them, the rest stay untouched.
    gl.pixelStorei(gl.PACK_ROW_LENGTH, 12);
    const off = new Float32Array(12 * 4 * 4).fill(7);
    gl.readPixels(W - 6, H - 2, 10, 4, gl.RGBA, gl.FLOAT, off);
    gl.pixelStorei(gl.PACK_ROW_LENGTH, 0);
    assert(gl.getError() === gl.NO_ERROR, 'clipped readPixels with PACK_ROW_LENGTH');
    let clippedOk = true;
    for (let r = 0; r < 4; r++) {
        for (let c = 0; c < 10; c++) {
            const x = W - 6 + c, y = H - 2 + r;
            for (let k = 0; k < 4; k++) {
                const got = off[(r * 12 + c) * 4 + k];
                const want = x < W && y < H ? src[(y * W + x) * 4 + k] : 7;
                if (got !== want) clippedOk = false;
            }
        }
    }
    assert(clippedOk, 'clipped same-format readPixels honours PACK_ROW_LENGTH and leaves outside texels alone');

    // --- the pack buffer ------------------------------------------------------
    const bytes = W * H * 16;
    const pbo = gl.createBuffer();
    gl.bindBuffer(gl.PIXEL_PACK_BUFFER, pbo);
    gl.bufferData(gl.PIXEL_PACK_BUFFER, bytes, gl.STREAM_READ);

    // A query left in the open command buffer: unavailable until submitted.
    const pendingQuery = () => {
        if (!timer) return null;
        const q = gl.createQuery();
        gl.beginQuery(timer.TIME_ELAPSED_EXT, q);
        gl.endQuery(timer.TIME_ELAPSED_EXT);
        return q;
    };
    const stillPending = (q, msg) => {
        if (!q) return;
        assert(gl.getQueryParameter(q, gl.QUERY_RESULT_AVAILABLE) === false, msg);
        gl.deleteQuery(q);
    };

    // 1. readPixels -> PBO, fence, poll on later frames, read: no stall.
    gl.readPixels(0, 0, W, H, gl.RGBA, gl.FLOAT, 0);
    const fence = gl.fenceSync(gl.SYNC_GPU_COMMANDS_COMPLETE, 0);
    gl.flush();
    let frames = 0;
    while (gl.clientWaitSync(fence, 0, 0) === gl.TIMEOUT_EXPIRED && frames < 200) {
        advanceTime(16);
        frames++;
    }
    const status = gl.clientWaitSync(fence, 0, 0);
    assert(status === gl.ALREADY_SIGNALED || status === gl.CONDITION_SATISFIED,
           'the fence signals within a few frames (polled ' + frames + ')');
    gl.deleteSync(fence);
    const q1 = pendingQuery();
    const viaPbo = new Float32Array(W * H * 4);
    gl.getBufferSubData(gl.PIXEL_PACK_BUFFER, 0, viaPbo);
    assert(gl.getError() === gl.NO_ERROR, 'getBufferSubData after the fence');
    sameAs(viaPbo, src, 'PBO contents after the fence match the target');
    stillPending(q1, 'getBufferSubData after a signalled fence did not submit the open command buffer');

    // 2. A partial range: just those bytes, then the whole buffer again.
    gl.clearBufferfv(gl.COLOR, 0, [1.5, -2.5, 3.5, -4.5]);
    gl.readPixels(0, 0, W, H, gl.RGBA, gl.FLOAT, 0);
    const fence2 = gl.fenceSync(gl.SYNC_GPU_COMMANDS_COMPLETE, 0);
    assert(gl.clientWaitSync(fence2, 0, 1e9) !== gl.WAIT_FAILED, 'second fence waited');
    gl.deleteSync(fence2);
    const part = new Float32Array(8);
    gl.getBufferSubData(gl.PIXEL_PACK_BUFFER, 5 * 16, part);
    sameAs(part, [1.5, -2.5, 3.5, -4.5, 1.5, -2.5, 3.5, -4.5], 'a partial range reads the cleared texels');
    const whole = new Float32Array(W * H * 4);
    gl.getBufferSubData(gl.PIXEL_PACK_BUFFER, 0, whole);
    let allCleared = true;
    for (let i = 0; i < whole.length; i++) if (whole[i] !== [1.5, -2.5, 3.5, -4.5][i % 4]) allCleared = false;
    assert(allCleared, 'the whole buffer after a partial read');

    // 3. No fence, but the write is complete (finish): the read copies on its
    // own submission and leaves the open command buffer alone.
    gl.clearBufferfv(gl.COLOR, 0, [9, 8, 7, 6]);
    gl.readPixels(0, 0, W, H, gl.RGBA, gl.FLOAT, 0);
    gl.finish();
    const q3 = pendingQuery();
    const after = new Float32Array(4);
    gl.getBufferSubData(gl.PIXEL_PACK_BUFFER, bytes - 16, after);
    sameAs(after, [9, 8, 7, 6], 'read after finish, no fence');
    stillPending(q3, 'a read of a buffer whose last write is complete did not submit the open command buffer');

    // 4. The write still unsubmitted: the read waits for it and is correct.
    gl.clearBufferfv(gl.COLOR, 0, [0.5, 0.25, 0.125, 1]);
    gl.readPixels(0, 0, W, H, gl.RGBA, gl.FLOAT, 0);
    const now = new Float32Array(4);
    gl.getBufferSubData(gl.PIXEL_PACK_BUFFER, 16, now);
    sameAs(now, [0.5, 0.25, 0.125, 1], 'read with the write still in the open command buffer');

    // 5. A CPU write after a GPU one keeps every byte: bufferSubData into the
    // middle, the GPU's bytes on either side.
    gl.clearBufferfv(gl.COLOR, 0, [2, 4, 6, 8]);
    gl.readPixels(0, 0, W, H, gl.RGBA, gl.FLOAT, 0);
    const fence5 = gl.fenceSync(gl.SYNC_GPU_COMMANDS_COMPLETE, 0);
    gl.clientWaitSync(fence5, 0, 1e9);
    gl.deleteSync(fence5);
    gl.bufferSubData(gl.PIXEL_PACK_BUFFER, 32, new Float32Array([-1, -1, -1, -1]));
    const mixed = new Float32Array(12);
    gl.getBufferSubData(gl.PIXEL_PACK_BUFFER, 16, mixed);
    sameAs(mixed, [2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8], 'CPU write between GPU-written texels');

    // 6. A GPU write after the fence's copy: the read sees the newer bytes.
    gl.clearBufferfv(gl.COLOR, 0, [3, 3, 3, 3]);
    gl.readPixels(0, 0, W, H, gl.RGBA, gl.FLOAT, 0);
    const fence6 = gl.fenceSync(gl.SYNC_GPU_COMMANDS_COMPLETE, 0);
    gl.clearBufferfv(gl.COLOR, 0, [5, 5, 5, 5]);
    gl.readPixels(0, 0, W, H, gl.RGBA, gl.FLOAT, 0);
    gl.clientWaitSync(fence6, 0, 1e9);
    gl.deleteSync(fence6);
    const newest = new Float32Array(4);
    gl.getBufferSubData(gl.PIXEL_PACK_BUFFER, 0, newest);
    sameAs(newest, [5, 5, 5, 5], 'a write after the fence is not hidden by the fence-time copy');

    // 7. Re-specifying the buffer drops what the GPU wrote.
    gl.bufferData(gl.PIXEL_PACK_BUFFER, bytes, gl.STREAM_READ);
    const zeros = new Float32Array(4).fill(1);
    gl.getBufferSubData(gl.PIXEL_PACK_BUFFER, 0, zeros);
    sameAs(zeros, [0, 0, 0, 0], 'bufferData replaces GPU-written contents');

    gl.bindBuffer(gl.PIXEL_PACK_BUFFER, null);
    assert(gl.getError() === gl.NO_ERROR, 'no GL error');
}
