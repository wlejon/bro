// The wrapper modules most apps never touch (host_js_modules.h,
// installDeferredModules) are entered when a program first reads one of
// their globals or `bro.<ns>` members, and then behave as if they had been
// there from the start.

function run() {
    var id = controlCommand.apply(null, arguments);
    for (var i = 0; i < 100; i++) {
        var r = controlResult(id);
        if (r) return r;
        advanceTime(16);
    }
    throw new Error('no reply to ' + Array.prototype.join.call(arguments, ' '));
}

function modulesLine() {
    var text = run('mem').payload;
    var line = text.split('\n').filter(function(l) { return l.indexOf('modules ') === 0; })[0];
    assert(line, 'mem reports the modules line:\n' + text);
    return line;
}
function deferred(label) {
    var line = modulesLine();
    var d = line.slice(line.indexOf('deferred:') + 9).trim().split(' ');
    return d.indexOf(label) >= 0;
}

// Nothing above this line has read a deferred name.
assert(deferred('3d'), 'the 3D family waits for a program: ' + modulesLine());
assert(deferred('net'), 'net waits for a program: ' + modulesLine());
assert(deferred('image.gpu'), 'bro.image.gpu waits for a program: ' + modulesLine());

// Listed without being entered: a key walk sees the members.
assert(Object.keys(bro).indexOf('scene') >= 0, 'bro.scene is listed');
assert('SceneGraph' in globalThis, 'SceneGraph is on globalThis');
assert(deferred('3d'), 'listing does not enter the 3D family');

// A bare global read enters the whole family.
assert(typeof SceneGraph === 'function', 'SceneGraph is a class: ' + typeof SceneGraph);
assert(!deferred('3d'), 'the read entered the 3D family: ' + modulesLine());
assert(typeof SceneNode === 'function', 'SceneNode came with it');
assert(typeof Physics === 'object' && typeof Physics.createWorld === 'function',
       'Physics is the wrapper js/physics.js builds');
assert(Physics.available === true, 'Physics is stamped available');
assert(typeof Terrain === 'function' && typeof Tween === 'function', 'the rest of the family is there');
assert(bro.scene && bro.scene.available === true, 'bro.scene is mounted and available');
assert(typeof bro.lighting.LightNode === 'function', 'bro.lighting is mounted');
assert(globalThis.SceneGraph === SceneGraph, 'globalThis.SceneGraph is the same class');

// A namespace member enters its group.
assert(typeof bro.image.gpu === 'object' && typeof bro.image.gpu.colormap === 'function',
       'bro.image.gpu is mounted on first read');
assert(!deferred('image.gpu'), 'reading bro.image.gpu entered it: ' + modulesLine());

if (deferred('net')) {
    assert(typeof bro.net.host === 'function', 'bro.net is mounted on first read');
    assert(typeof bro.net.sync === 'object' || typeof bro.net.sync === 'function',
           'bro.net.sync came with it: ' + typeof bro.net.sync);
    assert(bro.net.available !== undefined, 'bro.net says whether it is available');
    assert(!deferred('net'), 'reading bro.net entered it: ' + modulesLine());
}

// A scene context made by the canvas is a full SceneGraph.
var canvas = document.createElement('canvas');
document.body.appendChild(canvas);
var scene = canvas.getContext('scene');
if (scene) {
    assert(scene instanceof SceneGraph, 'getContext("scene") is a SceneGraph');
    assert(typeof scene.createBox === 'function' || typeof scene.createMesh === 'function',
           'with the members js/scene_graph.js installs');
}
