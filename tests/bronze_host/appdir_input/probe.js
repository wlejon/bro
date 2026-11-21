// The input probe: pointerLock, fullscreen, and navigator.getGamepads() in bronze_host.

function say(label, value) { console.log('APP ' + label + '=' + value); }

// ---------------------------------------------------------------------------
// 1. Brand Interfaces
// ---------------------------------------------------------------------------
say('brand.Gamepad', typeof Gamepad !== 'undefined' && Gamepad.name === 'Gamepad');
say('brand.GamepadButton', typeof GamepadButton !== 'undefined' && GamepadButton.name === 'GamepadButton');
say('brand.GamepadEvent', typeof GamepadEvent !== 'undefined' && GamepadEvent.name === 'GamepadEvent');

// ---------------------------------------------------------------------------
// 2. Pointer Lock API
// ---------------------------------------------------------------------------
say('pointerlock.initial', document.pointerLockElement === null);

const box = document.createElement('div');
box.id = 'target-box';
document.body.appendChild(box);

say('pointerlock.requestFn', typeof box.requestPointerLock === 'function');
say('pointerlock.exitFn', typeof document.exitPointerLock === 'function');

box.requestPointerLock();
say('pointerlock.locked', document.pointerLockElement === box);

document.exitPointerLock();
say('pointerlock.unlocked', document.pointerLockElement === null);

// ---------------------------------------------------------------------------
// 3. Fullscreen API
// ---------------------------------------------------------------------------
say('fullscreen.enabled', document.fullscreenEnabled === true);
say('fullscreen.initial', document.fullscreenElement === null);
say('fullscreen.requestFn', typeof box.requestFullscreen === 'function');
say('fullscreen.exitFn', typeof document.exitFullscreen === 'function');

const reqP = box.requestFullscreen();
say('fullscreen.reqReturnsPromise', reqP !== null && typeof reqP.then === 'function');
say('fullscreen.element', document.fullscreenElement === box);

const exitP = document.exitFullscreen();
say('fullscreen.exitReturnsPromise', exitP !== null && typeof exitP.then === 'function');
say('fullscreen.cleared', document.fullscreenElement === null);

// ---------------------------------------------------------------------------
// 4. Gamepad API (initial)
// ---------------------------------------------------------------------------
say('gamepad.getGamepadsFn', typeof navigator.getGamepads === 'function');
const initialPads = navigator.getGamepads();
say('gamepad.isArray', Array.isArray(initialPads));

// ---------------------------------------------------------------------------
// 5. Gamepad API (connected pad check via rAF)
// ---------------------------------------------------------------------------
requestAnimationFrame(function () {
    const pads = navigator.getGamepads();
    if (pads.length > 0 && pads[0] !== null) {
        const pad = pads[0];
        say('gamepad.connected', pad.connected === true);
        say('gamepad.id', pad.id);
        say('gamepad.index', pad.index);
        say('gamepad.mapping', pad.mapping);
        say('gamepad.buttonsCount', pad.buttons.length);
        say('gamepad.axesCount', pad.axes.length);
        say('gamepad.btn0Pressed', pad.buttons[0].pressed);
        say('gamepad.btn0Touched', pad.buttons[0].touched);
        say('gamepad.btn0Value', pad.buttons[0].value);
        say('gamepad.axis0', pad.axes[0]);
        say('gamepad.actuatorType', pad.vibrationActuator.type);
        say('gamepad.actuatorEffects', pad.vibrationActuator.effects.join(','));
        say('gamepad.actuatorPlayFn', typeof pad.vibrationActuator.playEffect === 'function');
        say('gamepad.actuatorResetFn', typeof pad.vibrationActuator.reset === 'function');

        pad.vibrationActuator.playEffect('dual-rumble', {
            duration: 100,
            strongMagnitude: 0.5,
            weakMagnitude: 0.25
        }).then(function (res) {
            say('gamepad.playEffectRes', res);
            return pad.vibrationActuator.reset();
        }).then(function (res) {
            say('gamepad.resetRes', res);
            say('probe.done', true);
        });
    } else {
        say('gamepad.connected', false);
        say('probe.done', true);
    }
});
