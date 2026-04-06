var canvases = [
    { el: document.getElementById('canvas-a'), color: '#ff4444', label: 'Red Circles', shape: 'circle' },
    { el: document.getElementById('canvas-b'), color: '#4444ff', label: 'Blue Squares', shape: 'rect' },
    { el: document.getElementById('canvas-c'), color: '#44ff44', label: 'Green Diamonds', shape: 'diamond' },
    { el: document.getElementById('canvas-d'), color: '#ffaa00', label: 'Orange Triangles', shape: 'triangle' },
];

canvases.forEach(function(c) {
    c.ctx = c.el.getContext('2d');
    console.log(c.label + ' context:', c.ctx ? 'OK' : 'FAILED');
});

function draw(c) {
    var ctx = c.ctx;
    var w = ctx.canvasWidth;
    var h = ctx.canvasHeight;
    ctx.clearRect(0, 0, w, h);
    ctx.fillStyle = '#12121a';
    ctx.fillRect(0, 0, w, h);

    ctx.fillStyle = c.color;
    for (var i = 0; i < 3; i++) {
        var x = 30 + i * 65;
        var y = h / 2;
        if (c.shape === 'circle') {
            ctx.beginPath();
            ctx.arc(x, y, 22, 0, Math.PI * 2);
            ctx.fill();
        } else if (c.shape === 'rect') {
            ctx.fillRect(x - 20, y - 20, 40, 40);
        } else if (c.shape === 'diamond') {
            ctx.beginPath();
            ctx.moveTo(x, y - 22);
            ctx.lineTo(x + 22, y);
            ctx.lineTo(x, y + 22);
            ctx.lineTo(x - 22, y);
            ctx.closePath();
            ctx.fill();
        } else if (c.shape === 'triangle') {
            ctx.beginPath();
            ctx.moveTo(x, y - 22);
            ctx.lineTo(x + 22, y + 18);
            ctx.lineTo(x - 22, y + 18);
            ctx.closePath();
            ctx.fill();
        }
    }

    ctx.fillStyle = '#fff';
    ctx.font = '12px sans-serif';
    ctx.fillText(c.label, 10, 18);
}

function render() {
    canvases.forEach(draw);
    requestAnimationFrame(render);
}

requestAnimationFrame(render);
console.log('Multi canvas test loaded - 4 canvases');
