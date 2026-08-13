advanceTime(32);

screenshot('wild_before.png');
console.log('DRV screenshot_before=1');

mouseDown(160, 120, 0);
mouseMove(200, 150);
mouseUp(200, 150, 0);
advanceTime(32);

wheel(160, 120, 12, 0);
advanceTime(32);

screenshot('wild_after.png');
console.log('DRV screenshot_after=1');

advanceTime(32);
console.log('DRV done=1');
