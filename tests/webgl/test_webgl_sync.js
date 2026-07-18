// WebGL2 conformance subset — sync objects: fenceSync, getSyncParameter,
// clientWaitSync with the MAX_CLIENT_WAIT_TIMEOUT_WEBGL cap, waitSync
// argument validation, isSync lifecycle. Completion is forced with finish()
// so the asserts are deterministic across drivers.
// Exercises src/js/webgl2_bindings_objects.cpp + src/webgl/webgl2_context.cpp.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '64');
canvas.setAttribute('height', '64');
document.body.appendChild(canvas);
flush();

const gl = canvas.getContext('webgl2');
if (!gl) {
    console.log('no webgl2; skipping');
} else {
    // =====================================================================
    // fenceSync + parameter queries
    // =====================================================================
    gl.clearColor(0.2, 0.4, 0.6, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    const sync = gl.fenceSync(gl.SYNC_GPU_COMMANDS_COMPLETE, 0);
    assert(sync !== null, 'fenceSync returns a sync object');
    assert(gl.isSync(sync) === true, 'isSync true for live sync');
    assert(gl.getSyncParameter(sync, gl.OBJECT_TYPE) === gl.SYNC_FENCE, 'OBJECT_TYPE is SYNC_FENCE');
    assert(gl.getSyncParameter(sync, gl.SYNC_CONDITION) === gl.SYNC_GPU_COMMANDS_COMPLETE,
           'SYNC_CONDITION round-trip');
    assert(gl.getSyncParameter(sync, gl.SYNC_FLAGS) === 0, 'SYNC_FLAGS is 0');
    assert(gl.getError() === gl.NO_ERROR, 'no error after fenceSync queries');

    // =====================================================================
    // Deterministic completion: finish() drains the pipeline, after which
    // the fence must be signaled and a 0-timeout wait already satisfied.
    // =====================================================================
    gl.finish();
    assert(gl.getSyncParameter(sync, gl.SYNC_STATUS) === gl.SIGNALED, 'SIGNALED after finish');
    const w0 = gl.clientWaitSync(sync, 0, 0);
    assert(w0 === gl.ALREADY_SIGNALED, 'clientWaitSync(0) after finish -> ALREADY_SIGNALED (got 0x' +
           w0.toString(16) + ')');

    // =====================================================================
    // Bounded polling pattern (the WebGL-portable idiom): flush + 0-timeout
    // clientWaitSync polls, wall-clock deadline as the safety net.
    // =====================================================================
    gl.clear(gl.COLOR_BUFFER_BIT);
    const sync2 = gl.fenceSync(gl.SYNC_GPU_COMMANDS_COMPLETE, 0);
    gl.flush();
    const deadline = Date.now() + 5000;
    let st = gl.clientWaitSync(sync2, gl.SYNC_FLUSH_COMMANDS_BIT, 0);
    while (st !== gl.ALREADY_SIGNALED && st !== gl.CONDITION_SATISFIED && Date.now() < deadline) {
        st = gl.clientWaitSync(sync2, 0, 0);
    }
    assert(st === gl.ALREADY_SIGNALED || st === gl.CONDITION_SATISFIED,
           'poll loop reached signaled state (got 0x' + st.toString(16) + ')');

    // =====================================================================
    // MAX_CLIENT_WAIT_TIMEOUT_WEBGL cap
    // =====================================================================
    const maxTimeout = gl.getParameter(gl.MAX_CLIENT_WAIT_TIMEOUT_WEBGL);
    assert(typeof maxTimeout === 'number' && maxTimeout >= 0, 'MAX_CLIENT_WAIT_TIMEOUT_WEBGL queryable');
    // A small in-cap timeout on a signaled fence is fine.
    assert(gl.clientWaitSync(sync2, 0, 1000) === gl.ALREADY_SIGNALED, 'small timeout on signaled fence');
    // Above the cap: INVALID_OPERATION + WAIT_FAILED, never an unbounded block.
    const wBig = gl.clientWaitSync(sync2, 0, maxTimeout * 10);
    assert(wBig === gl.WAIT_FAILED, 'over-cap timeout -> WAIT_FAILED');
    assert(gl.getError() === gl.INVALID_OPERATION, 'over-cap timeout -> INVALID_OPERATION');
    // Negative timeout: INVALID_VALUE.
    assert(gl.clientWaitSync(sync2, 0, -5) === gl.WAIT_FAILED, 'negative timeout -> WAIT_FAILED');
    assert(gl.getError() === gl.INVALID_VALUE, 'negative timeout -> INVALID_VALUE');

    // =====================================================================
    // waitSync: WebGL2 mandates flags == 0 and timeout == TIMEOUT_IGNORED
    // =====================================================================
    assert(gl.TIMEOUT_IGNORED === -1, 'TIMEOUT_IGNORED constant');
    gl.waitSync(sync2, 0, gl.TIMEOUT_IGNORED);
    assert(gl.getError() === gl.NO_ERROR, 'waitSync(0, TIMEOUT_IGNORED) is valid');
    gl.waitSync(sync2, 0, 0);
    assert(gl.getError() === gl.INVALID_VALUE, 'waitSync with numeric timeout -> INVALID_VALUE');

    // =====================================================================
    // Deletion semantics
    // =====================================================================
    gl.deleteSync(sync);
    assert(gl.isSync(sync) === false, 'isSync false after delete');
    gl.deleteSync(sync);   // double delete is a no-op
    gl.deleteSync(null);   // null is a no-op
    assert(gl.getError() === gl.NO_ERROR, 'no error after deletes');
    // Waiting on a deleted sync is INVALID_OPERATION, not a crash.
    assert(gl.clientWaitSync(sync, 0, 0) === gl.WAIT_FAILED, 'wait on deleted sync -> WAIT_FAILED');
    assert(gl.getError() === gl.INVALID_OPERATION, 'wait on deleted sync -> INVALID_OPERATION');
    gl.deleteSync(sync2);

    console.log('webgl sync tests passed');
}

document.body.removeChild(canvas);
