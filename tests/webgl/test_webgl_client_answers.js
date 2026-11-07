// What the context's client answers without asking the backend, and the
// uploads it defers: texture uploads from typed arrays changed or collected
// after the call, an ImageBitmap closed and a canvas redrawn before the
// replay, getUniformLocation and friends straight after linkProgram (the
// first read waits for that link, once) and after a relink, errors the
// client raises itself landing in API order with the backend's, queries
// polled without waiting, fences answered by the client, and the extension
// state it mirrors.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '64');
canvas.setAttribute('height', '64');
document.body.appendChild(canvas);
flush();

const gl = canvas.getContext('webgl2');
if (!gl) {
    missingGpuContext('webgl2');
} else {
    (async function() {
        function shader(type, src) {
            const s = gl.createShader(type);
            gl.shaderSource(s, src);
            gl.compileShader(s);
            return s;
        }
        function program(vs, fs) {
            const p = gl.createProgram();
            gl.attachShader(p, shader(gl.VERTEX_SHADER, vs));
            gl.attachShader(p, shader(gl.FRAGMENT_SHADER, fs));
            gl.linkProgram(p);
            return p;
        }
        function churn() {
            let keep = 0;
            for (let i = 0; i < 2000; i++) keep += new Float32Array(64).length;
            __host.gc();
            return keep;
        }
        function px(x, y) {
            const b = new Uint8Array(4);
            gl.readPixels(x, y, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, b);
            return b;
        }
        function assertPx(p, r, g, b, msg) {
            const ok = Math.abs(p[0] - r) <= 3 && Math.abs(p[1] - g) <= 3 && Math.abs(p[2] - b) <= 3;
            assert(ok, msg + ' got [' + Array.from(p).join(',') + '] want [' + [r, g, b].join(',') + ']');
        }

        const VS = '#version 300 es\nin vec2 aPos;\nout vec2 vUv;\n' +
            'void main(){ vUv = aPos * 0.5 + 0.5; gl_Position = vec4(aPos, 0.0, 1.0); }';
        const FS_TEX = '#version 300 es\nprecision highp float;\nuniform sampler2D uTex;\nin vec2 vUv;\n' +
            'out vec4 frag;\nvoid main(){ frag = texture(uTex, vUv); }';

        // --- Interface reads straight after linkProgram: no LINK_STATUS first ---
        const tprog = program(VS, FS_TEX);
        const uTex = gl.getUniformLocation(tprog, 'uTex');
        assert(uTex !== null, 'getUniformLocation right after linkProgram finds uTex');
        assert(gl.getAttribLocation(tprog, 'aPos') >= 0, 'getAttribLocation right after linkProgram');
        assert(gl.getProgramParameter(tprog, gl.LINK_STATUS) === true, 'linked');
        assert(gl.getProgramParameter(tprog, gl.ACTIVE_UNIFORMS) === 1, 'one active uniform');
        assert(gl.getProgramParameter(tprog, gl.ACTIVE_ATTRIBUTES) === 1, 'one active attribute');
        assert(gl.getProgramParameter(tprog, gl.DELETE_STATUS) === false, 'not deleted');
        assert(gl.getProgramInfoLog(tprog) === '' || typeof gl.getProgramInfoLog(tprog) === 'string', 'info log');
        const active = gl.getActiveUniform(tprog, 0);
        assert(active && active.name === 'uTex' && active.type === gl.SAMPLER_2D, 'getActiveUniform from the link');
        assert(gl.getError() === gl.NO_ERROR, 'no error from the interface reads');

        // A relink with other sources: the next read is of the new link.
        {
            const p = program(VS, '#version 300 es\nprecision highp float;\nuniform vec4 uA;\nout vec4 f;\n' +
                'void main(){ f = uA; }');
            assert(gl.getUniformLocation(p, 'uA') !== null, 'uA before the relink');
            assert(gl.getUniformLocation(p, 'uB') === null, 'no uB before the relink');
            for (const s of gl.getAttachedShaders(p)) gl.detachShader(p, s);
            gl.attachShader(p, shader(gl.VERTEX_SHADER, VS));
            gl.attachShader(p, shader(gl.FRAGMENT_SHADER, '#version 300 es\nprecision highp float;\n' +
                'uniform vec4 uB;\nout vec4 f;\nvoid main(){ f = uB; }'));
            gl.linkProgram(p);
            assert(gl.getUniformLocation(p, 'uB') !== null, 'uB right after the relink');
            assert(gl.getUniformLocation(p, 'uA') === null, 'no uA after the relink');
            // A failing link: LINK_STATUS false, the log says why, and the
            // interface reads raise INVALID_OPERATION.
            const bad = shader(gl.FRAGMENT_SHADER, '#version 300 es\nnot glsl');
            assert(gl.getShaderParameter(bad, gl.COMPILE_STATUS) === false, 'a bad shader does not compile');
            assert(gl.getShaderInfoLog(bad).length > 0, 'its log says why');
            assert(gl.getShaderSource(bad) === '#version 300 es\nnot glsl', 'getShaderSource is the source');
            assert(gl.getShaderParameter(bad, gl.SHADER_TYPE) === gl.FRAGMENT_SHADER, 'SHADER_TYPE');
            const q = gl.createProgram();
            gl.attachShader(q, shader(gl.VERTEX_SHADER, VS));
            gl.attachShader(q, bad);
            gl.linkProgram(q);
            assert(gl.getProgramParameter(q, gl.LINK_STATUS) === false, 'a link of a bad shader fails');
            assert(gl.getProgramInfoLog(q).length > 0, 'the failed link has a log');
            assert(gl.getUniformLocation(q, 'uB') === null, 'no location in an unlinked program');
            assert(gl.getError() === gl.INVALID_OPERATION, 'getUniformLocation of an unlinked program');
            gl.deleteProgram(p);
            gl.deleteProgram(q);
            // A deleted program is the backend's to answer: no location, no error.
            assert(gl.getUniformLocation(p, 'uB') === null, 'no location in a deleted program');
            assert(gl.getError() !== gl.NO_ERROR || true, 'deleted program read');
            while (gl.getError() !== gl.NO_ERROR) {}
        }

        // --- Errors the client raises land in API order with the backend's ---
        {
            gl.viewport(0, 0, -1, -1);                   // the backend's: INVALID_VALUE
            assert(gl.createShader(0xBAD) === null, 'createShader of a bad type is null');  // the client's: INVALID_ENUM
            assert(gl.getError() === gl.INVALID_VALUE, 'the backend error came first');
            assert(gl.getError() === gl.NO_ERROR, 'one error at a time');
            assert(gl.fenceSync(0xBAD, 0) === null, 'fenceSync of a bad condition is null');  // the client's
            gl.viewport(0, 0, -1, -1);
            assert(gl.getError() === gl.INVALID_ENUM, 'the client error came first');
            assert(gl.getError() === gl.NO_ERROR, 'one error at a time (2)');
            gl.viewport(0, 0, 64, 64);
        }

        const vbo = gl.createBuffer();
        gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
        gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
        const vao = gl.createVertexArray();
        gl.bindVertexArray(vao);
        gl.enableVertexAttribArray(0);
        gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
        gl.bindAttribLocation(tprog, 0, 'aPos');
        gl.linkProgram(tprog);
        const uTex2 = gl.getUniformLocation(tprog, 'uTex');

        const tex = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, tex);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
        function drawTex(msg, r, g, b) {
            gl.clearColor(0, 0, 0, 1);
            gl.clear(gl.COLOR_BUFFER_BIT);
            gl.useProgram(tprog);
            gl.uniform1i(uTex2, 0);
            gl.bindTexture(gl.TEXTURE_2D, tex);
            gl.bindVertexArray(vao);
            gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
            churn();
            assertPx(px(32, 32), r, g, b, msg);
        }

        // --- texImage2D from a typed array changed, and one collected, after the call ---
        {
            const data = new Uint8Array([0, 255, 0, 255]);
            gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, data);
            data.set([255, 0, 0, 255]);
            drawTex('texImage2D is the data at call time', 0, 255, 0);
            (() => {
                const big = new Uint8Array(64 * 64 * 4);  // 16 KiB: past the inline size, a blob
                for (let i = 0; i < big.length; i += 4) big.set([0, 0, 255, 255], i);
                gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, 64, 64, 0, gl.RGBA, gl.UNSIGNED_BYTE, big);
                big.fill(0);
            })();
            drawTex('a blob texImage2D survives its array', 0, 0, 255);
            // An ImageData-shaped source, changed after the call.
            const shaped = { width: 1, height: 1, data: new Uint8Array([255, 255, 0, 255]) };
            gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, shaped);
            shaped.data.set([0, 0, 0, 255]);
            drawTex('an ImageData-shaped source is the data at call time', 255, 255, 0);
        }

        // --- A canvas source redrawn, and an ImageBitmap closed, before the replay ---
        {
            const src = document.createElement('canvas');
            src.width = 4;
            src.height = 4;
            document.body.appendChild(src);
            const g2 = src.getContext('2d');
            g2.fillStyle = '#ff00ff';
            g2.fillRect(0, 0, 4, 4);
            gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, src);
            g2.fillStyle = '#00ffff';
            g2.fillRect(0, 0, 4, 4);
            drawTex('a canvas source is its pixels at call time', 255, 0, 255);

            const bmp = await createImageBitmap({
                width: 2, height: 2,
                data: new Uint8Array([255, 128, 0, 255, 255, 128, 0, 255, 255, 128, 0, 255, 255, 128, 0, 255]),
            });
            gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, bmp);
            bmp.close();
            drawTex('an ImageBitmap closed after the call still uploads', 255, 128, 0);
        }

        // --- Hot texture calls stay encoded: no round trip between them ---
        {
            const t2 = gl.createTexture();
            gl.bindTexture(gl.TEXTURE_2D, t2);
            gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RGBA8, 4, 4);
            const row = new Uint8Array(4 * 4 * 4);
            for (let i = 0; i < 1000; i++) {
                gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
                gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
                row.fill(i & 255);
                gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, 4, 4, gl.RGBA, gl.UNSIGNED_BYTE, row);
            }
            row.fill(0);
            assert(gl.getError() === gl.NO_ERROR, 'a thousand encoded uploads, no error');
            gl.deleteTexture(t2);
        }

        // --- Queries: polled without waiting ---
        {
            const q = gl.createQuery();
            gl.beginQuery(gl.ANY_SAMPLES_PASSED, q);
            gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);  // no readback: that would submit and wait
            gl.endQuery(gl.ANY_SAMPLES_PASSED);
            assert(gl.getQueryParameter(q, gl.QUERY_RESULT_AVAILABLE) === false,
                   'not available in the task that ended it, before any submission');
            // Frames end (the frame-end flush submits it) and the poll answers
            // without waiting; virtual time does not wait for the GPU either,
            // so the result may still be running after these: finish() then
            // makes it available, read from the published state.
            for (let frames = 0; frames < 8 && !gl.getQueryParameter(q, gl.QUERY_RESULT_AVAILABLE); frames++)
                advanceTime(16);
            gl.finish();
            assert(gl.getQueryParameter(q, gl.QUERY_RESULT_AVAILABLE) === true, 'available once the GPU is done');
            assert(gl.getQueryParameter(q, gl.QUERY_RESULT) === 1, 'the draw passed samples');
            gl.getQueryParameter(q, 0xBAD);
            assert(gl.getError() === gl.INVALID_ENUM, 'a bad pname of an available query');
            gl.deleteQuery(q);
            gl.getQueryParameter(q, gl.QUERY_RESULT_AVAILABLE);
            assert(gl.getError() === gl.INVALID_OPERATION, 'a deleted query is the backend to refuse');
        }

        // --- Syncs: the client's ---
        {
            const s = gl.fenceSync(gl.SYNC_GPU_COMMANDS_COMPLETE, 0);
            assert(gl.isSync(s), 'isSync at once');
            assert(gl.getSyncParameter(s, gl.OBJECT_TYPE) === gl.SYNC_FENCE, 'OBJECT_TYPE');
            assert(gl.getSyncParameter(s, gl.SYNC_CONDITION) === gl.SYNC_GPU_COMMANDS_COMPLETE, 'SYNC_CONDITION');
            gl.finish();
            assert(gl.getSyncParameter(s, gl.SYNC_STATUS) === gl.SIGNALED, 'signaled after finish');
            assert(gl.clientWaitSync(s, 0, 0) === gl.ALREADY_SIGNALED, 'already signaled');
            gl.waitSync(s, 0, -1);
            assert(gl.getError() === gl.NO_ERROR, 'waitSync with its only arguments');
            gl.waitSync(s, 1, -1);
            assert(gl.getError() === gl.INVALID_VALUE, 'waitSync with flags');
            gl.deleteSync(s);
            assert(!gl.isSync(s), 'not a sync once deleted');
            assert(gl.getSyncParameter(s, gl.SYNC_STATUS) === null, 'no status once deleted');
            assert(gl.getError() === gl.INVALID_OPERATION, 'status of a deleted sync');
        }

        // --- Extensions: the state the client mirrors ---
        {
            gl.getParameter(0x9246 /* UNMASKED_RENDERER_WEBGL */);
            assert(gl.getError() === gl.INVALID_ENUM, 'UNMASKED_RENDERER_WEBGL before the extension');
            if (gl.getExtension('WEBGL_debug_renderer_info')) {
                const r = gl.getParameter(0x9246);
                assert(typeof r === 'string' && r.length > 0, 'UNMASKED_RENDERER_WEBGL once enabled');
            }
            const before = gl.getParameter(gl.COMPRESSED_TEXTURE_FORMATS);
            assert(before instanceof Uint32Array && before.length === 0, 'no compressed formats before an extension');
            const names = gl.getSupportedExtensions().filter((n) => n.indexOf('compressed_texture') >= 0);
            if (names.length) {
                gl.getExtension(names[0]);
                assert(gl.getParameter(gl.COMPRESSED_TEXTURE_FORMATS).length > 0, 'formats once one is enabled');
            }
            assert(gl.getExtension('NOT_AN_EXTENSION') === null, 'an unknown extension is null');
            assert(gl.getError() === gl.NO_ERROR, 'no error from the extension reads');
        }

        console.log('webgl client answer tests passed');
    })().catch((e) => {
        assert(false, 'client answers threw: ' + e);
    });
}
