// SceneNode.drawOrder: the opaque pass draws low orders first. Two boxes
// at the same place have the same depths, so the depth test keeps whichever
// one the pass draws on its side of the tie: swapping their orders swaps the
// colour seen.

function centre(img) {
    const i = ((img.height >> 1) * img.width + (img.width >> 1)) * 4;
    return [img.data[i], img.data[i + 1], img.data[i + 2]];
}

const cv = document.createElement('canvas');
cv.setAttribute('width', '64');
cv.setAttribute('height', '64');
document.body.appendChild(cv);
flush();
const scene = cv.getContext('scene');
if (!scene) {
    missingGpuContext('scene');
} else {
    scene.setCamera({ fov: 60, near: 0.1, far: 100, position: [0, 0, 4], target: [0, 0, 0] });
    scene.setToneMap({ mode: 'linear', gamma: 1.0 });
    const flat = (rgb) => `uniform vec3 u_rgb;
        void userFragment(inout vec3 baseColor, inout vec3 normal, inout float metallic, inout float roughness,
                          inout vec3 emissive, inout float alpha) {
            baseColor = vec3(0.0); emissive = u_rgb;
        }`;
    const red = scene.createMesh({ mesh: 'box', color: 'white' });
    red.setShader({ fragment: flat(), uniforms: { u_rgb: [1, 0, 0] } });
    const blue = scene.createMesh({ mesh: 'box', color: 'white' });
    blue.setShader({ fragment: flat(), uniforms: { u_rgb: [0, 0, 1] } });

    assert(red.drawOrder === 10 && blue.drawOrder === 10, 'drawOrder is 10 by default');
    red.drawOrder = 5;
    const a = centre(scene.captureFrame());
    red.drawOrder = 20;
    const b = centre(scene.captureFrame());
    assert(red.drawOrder === 20, 'drawOrder reads back what was set');
    const redA = a[0] > 200 && a[2] < 50, redB = b[0] > 200 && b[2] < 50;
    const blueA = a[2] > 200 && a[0] < 50, blueB = b[2] > 200 && b[0] < 50;
    assert((redA && blueB) || (blueA && redB),
        `swapping the orders swaps the box seen (${a} then ${b})`);
    red.drawOrder = 999;
    assert(red.drawOrder === 255, 'drawOrder clamps to 255');
    console.log('draw order test passed');
}
