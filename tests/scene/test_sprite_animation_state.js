// A sprite's isPlaying / currentAnimation follow its sheet animation. The
// bronze natives answered only skinned meshes, so a walking sprite read
// isPlaying false and currentAnimation null.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '128');
canvas.setAttribute('height', '128');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
if (!scene) {
    console.log('no scene; skipping sprite animation state test');
} else {
    const s = scene.createSprite({
        sheet: { frameWidth: 16, frameHeight: 16, columns: 4, rows: 1 },
        animations: {
            walk: { frames: [0, 1, 2, 3], fps: 10, loop: true },
            hit: { frames: [0, 1, 2], fps: 10, loop: false },
        },
        play: 'walk',
    });
    assert(s.isPlaying === true, 'playing from the play option');
    assert(s.currentAnimation === 'walk', 'currentAnimation names it: ' + s.currentAnimation);
    advanceTime(250);
    assert(s.frameIndex === 2, 'frame advances: ' + s.frameIndex);
    assert(s.isPlaying === true, 'still playing while it loops');

    s.stop();
    assert(s.isPlaying === false, 'stop() pauses');
    s.play('hit');
    assert(s.isPlaying === true && s.currentAnimation === 'hit', 'play() switches: ' + s.currentAnimation);
    advanceTime(500);
    assert(s.isPlaying === false, 'a non-looping animation stops at its end');

    const still = scene.createSprite({ sheet: { frameWidth: 8, frameHeight: 8, columns: 2, rows: 1 } });
    assert(still.isPlaying === false && still.currentAnimation === null, 'a sprite with no animation: false / null');

    s.destroy();
    still.destroy();
    console.log('sprite animation state: ok');
}
