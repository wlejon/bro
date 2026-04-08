// Headless test for scene graph rendering
var c = document.getElementById('c');
console.log('canvas element:', c ? 'found' : 'null');

var scene = c.getContext('scene');
console.log('scene context:', scene ? 'got it' : 'null');

var rect = scene.createShape({
    shape: 'rect', width: 200, height: 200,
    fill: 'red', x: 400, y: 300
});
console.log('rect id:', rect.id, 'x:', rect.x, 'y:', rect.y);

scene.render();
console.log('render() called');

flush();
screenshot('scene-test-output.png');
console.log('screenshot saved');
