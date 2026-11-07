// node.setShaderAsync: the hook's stages compile off the page thread and the
// promise settles in a frame with the module set, drawing as setShader's
// does; several at once; a bad module rejects with setShader's Error and
// leaves the node as it was; a bad argument rejects with a TypeError.

function freshScene(size) {
    const cv = document.createElement('canvas');
    cv.setAttribute('width', String(size));
    cv.setAttribute('height', String(size));
    document.body.appendChild(cv);
    flush();
    const sc = cv.getContext('scene');
    if (sc) {
        sc.setDither(false);
        sc.setCamera({ fov: 60, near: 0.1, far: 100, position: [0, 0, 4], target: [0, 0, 0] });
        sc.setToneMap({ mode: 'linear', gamma: 1.0 });
        sc.setAmbient({ color: [0, 0, 0] });
    }
    return { canvas: cv, scene: sc };
}

function dropScene(s) {
    document.body.removeChild(s.canvas);
    flush();
}

function centerChannel(img, ch, r) {
    const cx = Math.floor(img.width / 2), cy = Math.floor(img.height / 2);
    let sum = 0, n = 0;
    for (let y = cy - r; y < cy + r; y++) {
        for (let x = cx - r; x < cx + r; x++) {
            sum += img.data[(y * img.width + x) * 4 + ch];
            n++;
        }
    }
    return n ? sum / n : 0;
}

const EMIT_MOD = `
module hook;
import mesh;

struct Params {
    tint: vec3<f32>,
}
@binding(4, 0) var params: uniform<Params>;

pub fn fragment(v: mesh::Vertex, s: mesh::Surface) -> mesh::Surface {
    var out = s;
    out.base_color = vec3<f32>(0.0);
    out.metallic = 0.0;
    out.roughness = 1.0;
    out.emissive = params.tint;
    return out;
}
`;

const DISPLACE_MOD = `
module hook;
import mesh;

pub fn vertex(v: mesh::Vertex) -> mesh::Vertex {
    var out = v;
    out.pos = vec3<f32>(v.pos.x, v.pos.y + 0.001, v.pos.z);
    return out;
}

pub fn fragment(v: mesh::Vertex, s: mesh::Surface) -> mesh::Surface {
    var out = s;
    out.base_color = vec3<f32>(0.0);
    out.roughness = 1.0;
    out.emissive = vec3<f32>(1.0, 0.0, 1.0);
    return out;
}
`;

const probe = freshScene(32);
if (!probe.scene) {
    missingGpuContext('scene');
} else {
    dropScene(probe);
    const SIZE = 96;

    // The same image as setShader's.
    const sync = freshScene(SIZE);
    const ms = sync.scene.createMesh({ mesh: 'box', color: '#ff0000' });
    ms.setShader({ module: EMIT_MOD, file: 'emit.bk', uniforms: { tint: [0, 1, 0] } });
    const want = sync.scene.captureFrame();
    dropScene(sync);

    const s = freshScene(SIZE);
    const m = s.scene.createMesh({ mesh: 'box', color: '#ff0000' });
    const p = m.setShaderAsync({ module: EMIT_MOD, file: 'emit_async.bk', uniforms: { tint: [0, 1, 0] } });
    assert(p instanceof Promise, 'setShaderAsync returns a Promise');
    assert(m.hasShader === false, 'the module is not set before the promise settles');
    const got = await p;
    assert(got === m, 'it resolves to the node');
    assert(m.hasShader === true, 'the module is set once it settles');
    const img = s.scene.captureFrame();
    assert(centerChannel(img, 1, 8) > 200 && centerChannel(img, 0, 8) < 20,
           `the hook draws (green ${centerChannel(img, 1, 8)})`);
    let maxDiff = 0;
    for (let i = 0; i < img.data.length; i++) maxDiff = Math.max(maxDiff, Math.abs(img.data[i] - want.data[i]));
    assert(maxDiff === 0, 'the same image as setShader gives (max channel difference ' + maxDiff + ')');

    // Several at once, one bad: the bad one rejects with setShader's Error and
    // leaves its node as it was.
    const a = s.scene.createMesh({ mesh: 'box', color: '#ffffff', position: [5, 0, 0] });
    const b = s.scene.createMesh({ mesh: 'sphere', color: '#ffffff', position: [-5, 0, 0] });
    const BAD = EMIT_MOD.replace('params.tint;', 'params.nope;');
    let syncErr = null;
    try {
        b.setShader({ module: BAD, file: 'bad.bk' });
    } catch (e) {
        syncErr = e;
    }
    assert(syncErr !== null, 'setShader refuses the bad module');
    const results = await Promise.allSettled([
        a.setShaderAsync({ module: DISPLACE_MOD, file: 'displace.bk' }),
        b.setShaderAsync({ module: BAD, file: 'bad.bk' }),
    ]);
    assert(results[0].status === 'fulfilled' && a.hasShader, 'the good hook is set');
    assert(results[1].status === 'rejected' && results[1].reason.message === syncErr.message,
           'the bad one rejects with setShader\'s message: ' + (results[1].reason && results[1].reason.message));
    assert(b.hasShader === false, 'and its node keeps no shader');

    let threw = false;
    let bad;
    try {
        bad = m.setShaderAsync(42);
    } catch (e) {
        threw = true;
    }
    assert(!threw, 'a bad argument does not throw');
    let typeErr = false;
    try {
        await bad;
    } catch (e) {
        typeErr = e instanceof TypeError;
    }
    assert(typeErr, 'a bad argument rejects with a TypeError');
    dropScene(s);
}
