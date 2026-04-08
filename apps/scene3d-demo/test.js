// Full test of all 4 demos
switchDemo(0);
advanceTime(50);
scene.render();
flush();
screenshot('scene3d-demo1.png');

switchDemo(1);
advanceTime(50);
scene.render();
flush();
screenshot('scene3d-demo2.png');

switchDemo(2);
advanceTime(50);
scene.render();
flush();
screenshot('scene3d-demo3.png');

switchDemo(3);
advanceTime(50);
scene.render();
flush();
screenshot('scene3d-demo4.png');

console.log('all demos saved');
