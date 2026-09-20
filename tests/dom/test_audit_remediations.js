// Tests verifying remediation of audit findings across bronze_host and engine.

// ---------------------------------------------------------------------------
// 1. Gamepad prototype getters
// ---------------------------------------------------------------------------
if (typeof GamepadButton !== 'undefined') {
    assert(typeof GamepadButton.prototype === 'object', 'GamepadButton.prototype exists');
    assert(GamepadButton.prototype.pressed === false, 'GamepadButton.prototype.pressed defaults to false');
    assert(GamepadButton.prototype.touched === false, 'GamepadButton.prototype.touched defaults to false');
    assert(GamepadButton.prototype.value === 0.0, 'GamepadButton.prototype.value defaults to 0.0');
}

if (typeof GamepadEvent !== 'undefined') {
    assert(typeof GamepadEvent.prototype === 'object', 'GamepadEvent.prototype exists');
    assert(GamepadEvent.prototype.gamepad === null, 'GamepadEvent.prototype.gamepad defaults to null');
}

// ---------------------------------------------------------------------------
// 2. TouchEvent & GestureEvent prototype inheritance
// ---------------------------------------------------------------------------
if (typeof TouchEvent !== 'undefined') {
    const te = new TouchEvent('touchstart', { cancelable: true });
    assert(te instanceof TouchEvent, 'te is TouchEvent');
    assert(te instanceof UIEvent, 'te inherits from UIEvent');
    assert(te instanceof Event, 'te inherits from Event');
    assert(typeof te.preventDefault === 'function', 'te has preventDefault()');
    assert(typeof te.stopPropagation === 'function', 'te has stopPropagation()');
    assert(typeof te.stopImmediatePropagation === 'function', 'te has stopImmediatePropagation()');
    assert(te.defaultPrevented === false, 'defaultPrevented starts false');
    te.preventDefault();
    assert(te.defaultPrevented === true, 'preventDefault flips defaultPrevented');
}

if (typeof GestureEvent !== 'undefined') {
    const ge = new GestureEvent('gesturestart', { cancelable: true });
    assert(ge instanceof GestureEvent, 'ge is GestureEvent');
    assert(ge instanceof UIEvent, 'ge inherits from UIEvent');
    assert(ge instanceof Event, 'ge inherits from Event');
    assert(typeof ge.preventDefault === 'function', 'ge has preventDefault()');
    assert(typeof ge.stopPropagation === 'function', 'ge has stopPropagation()');
    assert(typeof ge.stopImmediatePropagation === 'function', 'ge has stopImmediatePropagation()');
}

// ---------------------------------------------------------------------------
// 3. Web Animations global Animation
// ---------------------------------------------------------------------------
assert(typeof Animation === 'function', 'global Animation constructor exists');
assert(typeof window.Animation === 'function', 'window.Animation exists');
assert(typeof globalThis.Animation === 'function', 'globalThis.Animation exists');

// ---------------------------------------------------------------------------
// 5. WebGL2 invalidateSubFramebuffer
// ---------------------------------------------------------------------------
{
    const canvas = document.createElement('canvas');
    canvas.width = 128;
    canvas.height = 128;
    const gl = canvas.getContext('webgl2');
    if (gl) {
        assert(typeof gl.invalidateSubFramebuffer === 'function', 'gl.invalidateSubFramebuffer is a function');
        // Passing negative dimensions should set GL_INVALID_VALUE
        gl.invalidateSubFramebuffer(gl.FRAMEBUFFER, [gl.COLOR_ATTACHMENT0], 0, 0, -10, 10);
        assert(gl.getError() === gl.INVALID_VALUE, 'negative width generates INVALID_VALUE');

        gl.invalidateSubFramebuffer(gl.FRAMEBUFFER, [gl.COLOR_ATTACHMENT0], 0, 0, 10, -10);
        assert(gl.getError() === gl.INVALID_VALUE, 'negative height generates INVALID_VALUE');

        // Valid dimensions should succeed without error
        gl.invalidateSubFramebuffer(gl.FRAMEBUFFER, [gl.COLOR_ATTACHMENT0], 0, 0, 10, 10);
        assert(gl.getError() === gl.NO_ERROR, 'valid sub-framebuffer invalidation succeeds');
    }
}

// ---------------------------------------------------------------------------
// 4 & 7. Scene graph createPhysicsNode, JSON resilience & TileWorld loadGrid
// ---------------------------------------------------------------------------
{
    const canvas = document.createElement('canvas');
    canvas.width = 128;
    canvas.height = 128;
    document.body.appendChild(canvas);
    flush();

    const scene = canvas.getContext('scene');
    if (scene) {
        // createPhysicsNode with options
        if (typeof scene.createPhysicsNode === 'function') {
            const pnode = scene.createPhysicsNode({
                name: 'physics_test_node',
                autoSync: false,
                pixelsPerUnit: 50
            });
            assert(pnode !== null, 'createPhysicsNode returned a node');
            assert(pnode.name === 'physics_test_node', 'pnode name is preserved');

            // Bad JSON should be caught with LOG_WARN and not crash
            const badPhysics = scene.createPhysicsNode('invalid json syntax {{{');
            assert(badPhysics !== null, 'badPhysics node created despite bad JSON');
        }

        // Corrupted json options should not crash (handled via LOG_WARN)
        if (typeof scene.createShape === 'function') scene.createShape('invalid json syntax {{{');
        if (typeof scene.createSprite === 'function') scene.createSprite('invalid json syntax {{{');
        if (typeof scene.createParticles3D === 'function') scene.createParticles3D('invalid json syntax {{{');

        // TileWorld loadGrid test
        if (typeof scene.createTileWorld === 'function') {
            const world = scene.createTileWorld({
                width: 8,
                height: 8,
                layers: ['ground'],
                cellSize: 1.0,
                chunkSize: 4
            });
            if (world) {
                world.fillTile(0, 0, 7, 7, 1);
                const saved = world.save();
                assert(saved && saved.length > 0, 'world.save returns serialized grid');
                const ok = world.load(saved);
                assert(ok === true, 'world.load restores grid via loadGrid');
            }
        }
    }
    document.body.removeChild(canvas);
}

// ---------------------------------------------------------------------------
// 8. Document.prototype.elementFromPoint & elementsFromPoint
// ---------------------------------------------------------------------------
assert(typeof Document.prototype.elementFromPoint === 'function', 'Document.prototype.elementFromPoint exists');
assert(typeof Document.prototype.elementsFromPoint === 'function', 'Document.prototype.elementsFromPoint exists');
assert(typeof document.elementFromPoint === 'function', 'document.elementFromPoint exists');
assert(typeof document.elementsFromPoint === 'function', 'document.elementsFromPoint exists');

// Prototype method call with receiver
const hitViaProto = Document.prototype.elementFromPoint.call(document, 10, 10);
assert(hitViaProto !== null, 'Document.prototype.elementFromPoint.call returned an element');

console.log('PASS: test_audit_remediations.js');
