// Test Web Animations API (element.animate, Animation, getAnimations)
// Exercises src/js/web_animation_bindings.cpp

const div = document.createElement('div');
document.body.appendChild(div);
flush();

assert(typeof div.animate === 'function', 'element.animate exists');
assert(typeof div.getAnimations === 'function', 'element.getAnimations exists');
assert(typeof document.getAnimations === 'function', 'document.getAnimations exists');

// 1. Basic array keyframes and numeric duration
const anim1 = div.animate([
    { opacity: 0 },
    { opacity: 1 }
], 1000);

assert(anim1 !== null && typeof anim1 === 'object', 'animate returns Animation object');
assert(anim1.playState === 'running', 'initial playState is running');
assert(anim1.playbackRate === 1.0, 'default playbackRate is 1.0');
assert(anim1.pending === false, 'pending is false');

// 2. Playback control methods
anim1.pause();
assert(anim1.playState === 'paused', 'playState is paused after pause()');

anim1.play();
assert(anim1.playState === 'running', 'playState is running after play()');

anim1.playbackRate = 2.0;
assert(anim1.playbackRate === 2.0, 'playbackRate setter works');

anim1.currentTime = 500;
assert(anim1.currentTime === 500, 'currentTime setter works');

// 3. Object-of-arrays keyframe syntax and options object
const anim2 = div.animate({
    opacity: [0.2, 0.8],
    transform: ['translateX(0px)', 'translateX(100px)']
}, {
    duration: 500,
    delay: 50,
    iterations: 2,
    direction: 'alternate',
    fill: 'forwards',
    id: 'test-anim'
});

assert(anim2.id === 'test-anim', 'animation id is preserved');
anim2.id = 'updated-anim';
assert(anim2.id === 'updated-anim', 'animation id setter works');

// 4. getAnimations on element and document
const elAnims = div.getAnimations();
assert(Array.isArray(elAnims), 'element.getAnimations returns array');
assert(elAnims.length >= 2, 'element.getAnimations contains active animations');

const docAnims = document.getAnimations();
assert(Array.isArray(docAnims), 'document.getAnimations returns array');
assert(docAnims.length >= 2, 'document.getAnimations contains active animations');

// 5. finish() and finished promise resolution
let finishFired = false;
anim2.onfinish = () => { finishFired = true; };

anim2.finish();
assert(anim2.playState === 'finished', 'playState is finished after finish()');
assert(finishFired === true, 'onfinish event fired synchronously on finish()');

const finishedResult = await anim2.finished;
assert(finishedResult === anim2, 'finished promise resolved with Animation');

// 6. cancel() and finished promise rejection
const anim3 = div.animate([{ opacity: 0 }, { opacity: 1 }], 2000);
let cancelFired = false;
anim3.oncancel = () => { cancelFired = true; };

// Access finished promise before cancel to register rejection listener
const finishPromise3 = anim3.finished;
anim3.cancel();
assert(anim3.playState === 'idle', 'playState is idle after cancel()');
assert(cancelFired === true, 'oncancel event fired synchronously on cancel()');

let abortCaught = false;
try {
    await finishPromise3;
} catch (e) {
    abortCaught = true;
    assert(e.name === 'AbortError', 'finished promise rejected with AbortError');
}
assert(abortCaught === true, 'finished promise rejected on cancel');

// 7. reverse()
const anim4 = div.animate([{ opacity: 0 }, { opacity: 1 }], 1000);
anim4.reverse();
assert(anim4.playbackRate === -1.0, 'playbackRate inverted after reverse()');
anim4.cancel();

// Cleanup
anim1.cancel();
anim2.cancel();
document.body.removeChild(div);

console.log('test_web_animations: passed');
