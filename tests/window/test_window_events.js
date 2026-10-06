// bro.window — focus/blur events, system bell, progress, notifications.

assert(typeof bro === 'object', 'bro namespace exists');
assert(typeof bro.window === 'object', 'bro.window exists');

// ---- Focus and Blur Events --------------------------------------------------
let focusCount = 0;
let blurCount = 0;
let lastEventType = '';

function onFocusListener(e) {
    focusCount++;
    lastEventType = e.type;
}

function onBlurListener(e) {
    blurCount++;
    lastEventType = e.type;
}

bro.window.addEventListener('focus', onFocusListener);
bro.window.addEventListener('blur', onBlurListener);

let propFocusFired = false;
let propBlurFired = false;
bro.window.onfocus = (e) => {
    propFocusFired = true;
    assert(e.target === bro.window, 'event.target is bro.window');
};
bro.window.onblur = (e) => {
    propBlurFired = true;
    assert(e.target === bro.window, 'event.target is bro.window');
};

// Simulate blur
__bro_native.window.simulateFocus(false);
assert(blurCount === 1, 'blur listener called once, got ' + blurCount);
assert(lastEventType === 'blur', 'last event was blur');
assert(propBlurFired === true, 'onblur handler called');
assert(bro.window.focused === false, 'focused is false after blur');

// Simulate focus
__bro_native.window.simulateFocus(true);
assert(focusCount === 1, 'focus listener called once, got ' + focusCount);
assert(lastEventType === 'focus', 'last event was focus');
assert(propFocusFired === true, 'onfocus handler called');
assert(bro.window.focused === true, 'focused is true after focus');

// Test removeEventListener
bro.window.removeEventListener('focus', onFocusListener);
bro.window.removeEventListener('blur', onBlurListener);
__bro_native.window.simulateFocus(false);
assert(blurCount === 1, 'removed blur listener not called again');
__bro_native.window.simulateFocus(true);
assert(focusCount === 1, 'removed focus listener not called again');

// ---- System Bell (beep) ------------------------------------------------------
assert(typeof bro.window.beep === 'function', 'beep function exists');
__bro_native.window.resetBeepCount();
assert(__bro_native.window.getBeepCount() === 0, 'beep count starts at 0');

assert(bro.window.beep() === true, 'beep() returns true');
assert(__bro_native.window.getBeepCount() === 1, 'beep count incremented to 1');

bro.window.beep();
bro.window.beep();
assert(__bro_native.window.getBeepCount() === 3, 'beep count incremented to 3');

__bro_native.window.resetBeepCount();
assert(__bro_native.window.getBeepCount() === 0, 'beep count reset to 0');

// ---- Taskbar Progress --------------------------------------------------------
assert(typeof bro.window.setProgress === 'function', 'setProgress function exists');

// States: 'none' (0), 'normal' (1), 'error' (2), 'indeterminate' (3), 'paused' (4)
bro.window.setProgress('normal', 42);
assert(__bro_native.window.getProgressState() === 1, 'progress state normal is 1');
assert(__bro_native.window.getProgressValue() === 42, 'progress value is 42');

bro.window.setProgress('paused', 80);
assert(__bro_native.window.getProgressState() === 4, 'progress state paused is 4');
assert(__bro_native.window.getProgressValue() === 80, 'progress value is 80');

bro.window.setProgress('error', 99);
assert(__bro_native.window.getProgressState() === 2, 'progress state error is 2');
assert(__bro_native.window.getProgressValue() === 99, 'progress value is 99');

bro.window.setProgress('indeterminate', 0);
assert(__bro_native.window.getProgressState() === 3, 'progress state indeterminate is 3');

bro.window.setProgress('none', 0);
assert(__bro_native.window.getProgressState() === 0, 'progress state none is 0');
assert(__bro_native.window.getProgressValue() === 0, 'progress value is 0');

// Value clamping: <0 -> 0, >100 -> 100
bro.window.setProgress('normal', -10);
assert(__bro_native.window.getProgressValue() === 0, 'progress value clamped to 0');
bro.window.setProgress('normal', 150);
assert(__bro_native.window.getProgressValue() === 100, 'progress value clamped to 100');

// ---- Desktop Notifications ---------------------------------------------------
assert(typeof bro.window.notify === 'function', 'notify function exists');
__bro_native.window.clearNotifications();
assert(__bro_native.window.getNotificationCount() === 0, 'notification count starts at 0');

let notifId = bro.window.notify('Build Complete', 'All 42 tests passed!', {
    icon: 'app://icon.png',
    timeout: 5000,
    silent: true,
});
assert(typeof notifId === 'number' && notifId > 0, 'notify returns positive id: ' + notifId);
assert(__bro_native.window.getNotificationCount() === 1, 'notification count is 1');
assert(__bro_native.window.getLastNotificationTitle() === 'Build Complete', 'notification title matches');
assert(__bro_native.window.getLastNotificationBody() === 'All 42 tests passed!', 'notification body matches');

bro.window.notify('Update Available', 'Version 2.0 is out.');
assert(__bro_native.window.getNotificationCount() === 2, 'notification count is 2');
assert(__bro_native.window.getLastNotificationTitle() === 'Update Available', 'second notification title matches');

__bro_native.window.clearNotifications();
assert(__bro_native.window.getNotificationCount() === 0, 'notification count cleared');

console.log('bro.window events OK');
