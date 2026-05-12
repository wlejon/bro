// Test bro.scene basic API — SceneGraph via canvas.getContext('scene').
// Exercises src/js/scene_bindings.cpp factory paths and SceneNode properties.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '256');
canvas.setAttribute('height', '256');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
if (!scene) {
    // No GPU available — skip silently
    console.log('scene context not available (no GPU)');
} else {
    // =====================================================================
    // Camera
    // =====================================================================
    scene.setCamera({
        fov: 60, near: 0.1, far: 100,
        position: [2, 2, 2], target: [0, 0, 0], up: [0, 1, 0],
    });

    // Ambient and clear
    if (typeof scene.setClearColor === 'function')
        scene.setClearColor(0.1, 0.1, 0.1, 1);

    // =====================================================================
    // Mesh primitives
    // =====================================================================
    const box = scene.createMesh({ mesh: 'box', color: 'red' });
    assert(box !== null && box !== undefined, 'createMesh returns node');
    assert(box.type === 'mesh', 'mesh type');

    box.position = [1, 0, 0];
    const pos = box.position;
    assert(Array.isArray(pos), 'position array');
    assert(Math.abs(pos[0] - 1) < 0.01, 'position.x = 1');

    box.x = 2; box.y = 3; box.z = 4;
    assert(Math.abs(box.x - 2) < 0.01, 'x setter');
    assert(Math.abs(box.y - 3) < 0.01, 'y setter');
    assert(Math.abs(box.z - 4) < 0.01, 'z setter');

    box.visible = false;
    assert(box.visible === false, 'visible setter');
    box.visible = true;

    box.name = 'mybox';
    assert(box.name === 'mybox', 'name setter');

    const sphere = scene.createMesh({ mesh: 'sphere', color: 'blue', radius: 0.5 });
    assert(sphere.type === 'mesh', 'sphere is mesh');

    const cyl = scene.createMesh({ mesh: 'cylinder', color: 'green' });
    assert(cyl !== null, 'cylinder mesh');

    // =====================================================================
    // Shape (2D)
    // =====================================================================
    const shape = scene.createShape();
    assert(shape !== null, 'createShape');
    assert(shape.type === 'shape', 'shape type');

    // =====================================================================
    // Sprite
    // =====================================================================
    const sprite = scene.createSprite();
    assert(sprite !== null, 'createSprite');
    assert(sprite.type === 'sprite', 'sprite type');

    // =====================================================================
    // Light
    // =====================================================================
    const light = scene.createLight({
        type: 'directional',
        direction: [1, -1, 0],
        color: [1, 1, 1],
        intensity: 1.0,
    });
    assert(light !== null, 'createLight');
    assert(light.type === 'light', 'light type');

    const pt = scene.createLight({ type: 'point', position: [0, 5, 0] });
    assert(pt !== null, 'point light');

    // =====================================================================
    // Hierarchy
    // =====================================================================
    box.add(sphere);
    assert(box.childCount >= 1, 'box has 1 child');

    if (typeof box.remove === 'function') box.remove(sphere);

    // =====================================================================
    // Transform setters
    // =====================================================================
    box.rotationX = 0.5;
    box.rotationY = 0.5;
    box.rotationZ = 0.5;
    if (typeof box.setRotation === 'function') {
        box.setRotation(0, 0, 0, 1);
    }

    if (typeof box.scale === 'object' || typeof box.scale === 'function') {
        try { box.scale = [2, 2, 2]; } catch(e) {}
    }

    // =====================================================================
    // Ambient and rendering
    // =====================================================================
    if (typeof scene.setAmbient === 'function') {
        scene.setAmbient(0.2, 0.2, 0.2);
    }

    // Force a frame
    flush();
}

// Cleanup
document.body.removeChild(canvas);
