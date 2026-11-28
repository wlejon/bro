// Headless test for native `.bk` ES module imports, exportNative MIR fusion,
// StructView.exportFieldNative, bro.kernel.batch(), and KernelBuffer.readAsync().
import esMod, { df_shift, store_accum, scale_buf, eval_pair, __source } from './_es_fixture.bk';

assert(typeof bro.kernel === 'object', 'bro.kernel namespace exists');
if (!bro.kernel.available) {
    skipTest('bro.kernel is compiled out of this build');
} else {
    assert(esMod && typeof __source === 'string' && __source.includes('df_shift'), '.bk default and __source exports');

    // 1. Imported df32 kernel and pub @fn via ES module bindings
    const dRet = df_shift.eval(0.062623456789);
    assert(Math.abs(dRet - 0.062623456789) < 1e-10, 'imported df_shift.eval: ' + dRet);
    const pRet = eval_pair.bind(undefined, 3.5)(2.0);
    assert(Math.abs(pRet - 7.5) < 1e-5, 'imported pub fn bind with transitive .bk helper: ' + pRet);

    // 2. Native MIR export on store+scalar+helper kernel and StructView field reader
    store_accum.exportNative('bro.test.esStoreAccum');
    df_shift.exportNative('bro.test.esDfShift');

    const stride = esMod.structLayout('EsCell').byteSize;
    const svBuf = bro.kernel.createBuffer(stride * 2, { residency: 'host' });
    const sv = esMod.structView('EsCell', svBuf);
    sv.setStruct(0, { weight: 2.5, height: 16384.1250625, tag: 42 });
    sv.exportFieldNative('height', 'bro.test.esCellHeight');
    sv.exportFieldNative('tag', 'bro.test.esCellTag');

    // Compile a dynamic function after exportNative so Bronze fuses the registered MIR natives.
    const arr = new Float32Array([1.0, 2.0, 3.0]);
    const runFused = new Function('arr', 'bytes', `
        const accumOut = bro.test.esStoreAccum(arr, 1.0, 3);
        const dfOut = bro.test.esDfShift(0.062623456789);
        const h0 = bro.test.esCellHeight(bytes, 0);
        const t0 = bro.test.esCellTag(bytes, 0);
        const tOob = bro.test.esCellTag(bytes, 99);
        return { accumOut, dfOut, h0, t0, tOob };
    `);
    const fusedRes = runFused(arr, sv.bytes);
    // sum_i (2*arr[i] + 1) = (3 + 5 + 7) = 15
    assert(Math.abs(fusedRes.accumOut - 15.0) < 1e-6, 'exportNative store_accum: ' + fusedRes.accumOut);
    assert(Math.abs(fusedRes.dfOut - 0.062623456789) < 1e-10, 'exportNative df_shift: ' + fusedRes.dfOut);
    assert(Math.abs(fusedRes.h0 - 16384.1250625) < 1e-6, 'StructView df32 height: ' + fusedRes.h0);
    assert(fusedRes.t0 === 42 && fusedRes.tOob === 0, 'StructView tag + OOB');

    // 3. Host KernelBuffer.readAsync()
    const hostBuf = bro.kernel.createBuffer(12, { residency: 'host' });
    hostBuf.write(new Float32Array([4.0, 5.0, 6.0]));
    const hostArr = await hostBuf.readAsync(Float32Array);
    assert(hostArr instanceof Float32Array && hostArr.length === 3 && hostArr[1] === 5.0, 'host readAsync');

    // 4. GPU bro.kernel.batch() + GPU KernelBuffer.readAsync() when GPU device exists
    if (bro.kernel.gpu) {
        const inp = bro.kernel.createBuffer(new Float32Array([1.0, 2.0, 3.0, 4.0]));
        const mid = bro.kernel.createBuffer(16);
        const out = bro.kernel.createBuffer(16);

        bro.kernel.batch(() => {
            scale_buf.dispatch([4], inp, mid, 2.0);
            scale_buf.dispatch([4], mid, out, 0.5);
        });

        const gpuArr = await out.readAsync(Float32Array);
        // mid = [6, 10, 14, 18]; out = ((2*mid + 1) * 0.5) = [6.5, 10.5, 14.5, 18.5]
        const expect = [6.5, 10.5, 14.5, 18.5];
        assert(gpuArr.length === 4, 'gpu readAsync length');
        for (let i = 0; i < 4; ++i) {
            assert(Math.abs(gpuArr[i] - expect[i]) < 1e-4, 'gpu batch+readAsync [' + i + ']=' + gpuArr[i]);
        }
    }
}
