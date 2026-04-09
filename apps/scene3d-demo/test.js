// Render demo 1 after several frames to verify no paint accumulation
switchDemo(0);
for (var i = 0; i < 10; i++) {
    advanceTime(16);
}
flush();
screenshot('scene3d-final.png');
console.log('final screenshot saved');
