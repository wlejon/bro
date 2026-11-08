// WebGL constants are WebIDL constants (WebIDL 3.7.3): on both
// WebGL2RenderingContext and its prototype, each one
// {writable: false, enumerable: true, configurable: false}. An assignment is
// discarded in sloppy code and a TypeError in strict code, a delete answers
// false, and a redefinition to another value throws. Reads through a context
// keep answering the constant after all of that, and after the context grows
// a shadowing own property (which then answers instead).

const NAMES = ['FLOAT', 'RGBA', 'TEXTURE_2D', 'ARRAY_BUFFER', 'DEPTH_BUFFER_BIT', 'INVALID_INDEX', 'TIMEOUT_IGNORED'];
const VALUES = {
    FLOAT: 0x1406, RGBA: 0x1908, TEXTURE_2D: 0x0DE1, ARRAY_BUFFER: 0x8892,
    DEPTH_BUFFER_BIT: 0x100, INVALID_INDEX: 4294967295, TIMEOUT_IGNORED: -1,
};

function checkDescriptor(holder, where, name) {
    const d = Object.getOwnPropertyDescriptor(holder, name);
    assert(d !== undefined, `${where}.${name} is an own property`);
    assert(d.value === VALUES[name], `${where}.${name} === ${VALUES[name]}: ${d.value}`);
    assert(d.writable === false, `${where}.${name} is not writable`);
    assert(d.enumerable === true, `${where}.${name} is enumerable`);
    assert(d.configurable === false, `${where}.${name} is not configurable`);
    assert(!('get' in d) && !('set' in d), `${where}.${name} is a data property`);
}

const iface = WebGL2RenderingContext;
const proto = WebGL2RenderingContext.prototype;
for (const name of NAMES) {
    checkDescriptor(iface, 'WebGL2RenderingContext', name);
    checkDescriptor(proto, 'WebGL2RenderingContext.prototype', name);
}

// Every constant: the same value on both, both read-only and permanent.
let constants = 0;
for (const name of Object.getOwnPropertyNames(proto)) {
    if (!/^[A-Z][A-Z0-9_]*$/.test(name)) continue;
    const p = Object.getOwnPropertyDescriptor(proto, name);
    const i = Object.getOwnPropertyDescriptor(iface, name);
    assert(i !== undefined && i.value === p.value, `${name} is on the interface too, with the same value`);
    assert(!p.writable && p.enumerable && !p.configurable, `${name} on the prototype is a constant`);
    assert(!i.writable && i.enumerable && !i.configurable, `${name} on the interface is a constant`);
    constants++;
}
assert(constants > 500, `the prototype carries the WebGL2 constants: ${constants}`);

// Sloppy assignment: discarded, no throw.
proto.FLOAT = 1;
iface.FLOAT = 1;
assert(proto.FLOAT === 0x1406 && iface.FLOAT === 0x1406, 'a sloppy assignment is discarded');

// Strict assignment: a TypeError, the value kept.
function strictAssign(target, name) {
    'use strict';
    try {
        target[name] = 7;
    } catch (e) {
        return e instanceof TypeError;
    }
    return false;
}
function strictAssignNamed(target) {
    'use strict';
    try {
        target.RGBA = 7;
    } catch (e) {
        return e instanceof TypeError;
    }
    return false;
}
assert(strictAssign(proto, 'FLOAT'), 'a strict assignment to the prototype throws a TypeError');
assert(strictAssign(iface, 'FLOAT'), 'a strict assignment to the interface throws a TypeError');
assert(strictAssignNamed(proto), 'a strict named assignment throws a TypeError');
assert(proto.FLOAT === 0x1406 && proto.RGBA === 0x1908, 'the strict assignments changed nothing');

// Delete and redefinition.
assert(delete proto.FLOAT === false, 'delete answers false');
assert(proto.FLOAT === 0x1406, 'delete changed nothing');
let redefineThrew = false;
try {
    Object.defineProperty(proto, 'FLOAT', { value: 3 });
} catch (e) {
    redefineThrew = e instanceof TypeError;
}
assert(redefineThrew, 'redefining to another value is a TypeError');
Object.defineProperty(proto, 'FLOAT', { value: 0x1406 });  // SameValue: allowed

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '4');
canvas.setAttribute('height', '4');
document.body.appendChild(canvas);
flush();

const gl = canvas.getContext('webgl2');
if (!gl) {
    missingGpuContext('webgl2');
} else {
    function readFloat(c) { return c.FLOAT; }
    function readRgba(c) { return c.RGBA; }
    let sum = 0;
    for (let i = 0; i < 100; i++) sum += readFloat(gl) + readRgba(gl);
    assert(sum === 100 * (0x1406 + 0x1908), `reads through the context: ${sum}`);
    assert(gl.INVALID_INDEX === 4294967295 && gl.TIMEOUT_IGNORED === -1, 'the out-of-int32 constants read back');

    // Through the context: sloppy discarded, strict throws, nothing own.
    gl.FLOAT = 1;
    assert(readFloat(gl) === 0x1406, 'a sloppy assignment through the context is discarded');
    assert(!Object.prototype.hasOwnProperty.call(gl, 'FLOAT'), 'and makes no own property');
    assert(strictAssign(gl, 'FLOAT'), 'a strict assignment through the context throws a TypeError');
    assert(readFloat(gl) === 0x1406, 'and changes nothing');

    // A shadowing own property, after the read site has seen the constant.
    const other = document.createElement('canvas').getContext('webgl2');
    Object.defineProperty(gl, 'FLOAT', { value: 42, configurable: true });
    let shadowSum = 0;
    for (let i = 0; i < 10; i++) shadowSum += readFloat(gl);
    assert(shadowSum === 420, `the own property answers: ${shadowSum}`);
    if (other) assert(readFloat(other) === 0x1406, 'another context still reads the constant');
    delete gl.FLOAT;
    assert(readFloat(gl) === 0x1406, 'the constant again once the own property is gone');
}
