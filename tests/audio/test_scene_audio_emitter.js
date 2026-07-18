// Scene-attached audio emitters + camera-bound listener: the engine syncs
// node world positions (and finite-difference velocities feeding Doppler)
// into broaudio every frame — zero per-frame JS. Deterministic via
// advanceTime: a node moving toward the listener yields a Doppler ratio > 1
// (rising perceived pitch), moving away < 1; detach freezes the sync; a
// destroyed node self-prunes; the camera binding drives listener position.

const ctx = new AudioContext();
const sr = ctx.sampleRate;

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '128');
canvas.setAttribute('height', '128');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
assert(scene !== null, 'scene context');

// Listener at origin looking down -Z (broaudio default), bound to the camera.
scene.setCamera({ position: [0, 0, 0], target: [0, 0, -1] });
scene.bindAudioListenerToCamera(true);

const tone = new Float32Array(sr);
for (let i = 0; i < tone.length; i++) tone[i] = 0.5 * Math.sin(2 * Math.PI * 440 * i / sr);
const clip = ctx.createClip(tone, 1);

// --- approaching node -> ratio > 1 ------------------------------------------
{
    const node = scene.createNode('emitter');
    node.position = [0, 0, -40];
    const id = ctx.playClip(clip, 1.0, true);
    node.attachAudioEmitter(id);

    // Move the node toward the camera at 20 u/s: 16 ms steps, 0.32 u each.
    let z = -40;
    for (let i = 0; i < 12; i++) {
        z += 20 * 0.016;
        node.position = [0, 0, z];
        advanceTime(16);
    }
    const ratio = ctx.getPlaybackDopplerRatio(id);
    console.log('approach ratio:', ratio);
    // Expected ~343/(343-20) = 1.062; generous window for step quantization.
    assert(ratio > 1.02 && ratio < 1.12, 'approaching emitter raises pitch, got ' + ratio);

    // --- reverse direction -> ratio < 1 --------------------------------------
    for (let i = 0; i < 12; i++) {
        z -= 20 * 0.016;
        node.position = [0, 0, z];
        advanceTime(16);
    }
    const ratio2 = ctx.getPlaybackDopplerRatio(id);
    console.log('recede ratio:', ratio2);
    assert(ratio2 < 0.98 && ratio2 > 0.90, 'receding emitter lowers pitch, got ' + ratio2);

    // --- attenuation follows the node ---------------------------------------
    // Note the spatial gain: nearer must be louder than far.
    node.position = [0, 0, -2];
    advanceTime(50);
    const nearPeak = Math.max(ctx.getBusPeakL(0), ctx.getBusPeakR(0));
    node.position = [0, 0, -80];
    advanceTime(100);
    const farPeak = Math.max(ctx.getBusPeakL(0), ctx.getBusPeakR(0));
    console.log('near peak:', nearPeak, 'far peak:', farPeak);
    assert(nearPeak > farPeak, 'engine-synced position drives attenuation');

    // --- detach freezes the sync (audio keeps playing) -----------------------
    node.detachAudioEmitter();
    node.position = [0, 0, -2];
    advanceTime(50);
    const detachedPeak = Math.max(ctx.getBusPeakL(0), ctx.getBusPeakR(0));
    assert(detachedPeak < nearPeak * 0.5,
        'after detach the source stays far (no sync), got ' + detachedPeak);

    ctx.stopPlayback(id);
    scene.destroyNode(node);
}

// --- destroyed node self-prunes; dead handle is a safe no-op -----------------
{
    const node = scene.createNode('shortlived');
    node.position = [0, 0, -5];
    const id = ctx.playClip(clip, 1.0, true);
    node.attachAudioEmitter(id);
    advanceTime(32);
    ctx.stopPlayback(id);   // dead handle: sync becomes a no-op
    advanceTime(32);
    scene.destroyNode(node);    // binding self-prunes
    advanceTime(32);        // no crash = pass
}

// --- camera binding drives the listener --------------------------------------
{
    const node = scene.createNode('static');
    node.position = [0, 0, -30];
    const id = ctx.playClip(clip, 1.0, true);
    node.attachAudioEmitter(id);
    advanceTime(50);
    const farPeak = Math.max(ctx.getBusPeakL(0), ctx.getBusPeakR(0));

    // Fly the camera next to the emitter — loudness must rise.
    scene.setCamera({ position: [0, 0, -28], target: [0, 0, -30] });
    advanceTime(100);
    const nearPeak = Math.max(ctx.getBusPeakL(0), ctx.getBusPeakR(0));
    console.log('listener far peak:', farPeak, 'near peak:', nearPeak);
    assert(nearPeak > farPeak, 'camera-bound listener moved toward the emitter');

    // Camera motion also produces listener-side Doppler: step toward the
    // emitter and the ratio rises above 1.
    let camZ = -20;
    scene.setCamera({ position: [0, 0, camZ], target: [0, 0, -30] });
    advanceTime(16);
    for (let i = 0; i < 12; i++) {
        camZ -= 15 * 0.016;
        scene.setCamera({ position: [0, 0, camZ], target: [0, 0, -30] });
        advanceTime(16);
    }
    const ratio = ctx.getPlaybackDopplerRatio(id);
    console.log('listener-motion ratio:', ratio);
    assert(ratio > 1.01, 'listener motion toward emitter raises pitch, got ' + ratio);

    ctx.stopPlayback(id);
    scene.destroyNode(node);
    scene.bindAudioListenerToCamera(false);
}

console.log('test_scene_audio_emitter: all assertions passed');
