const canvas = document.getElementById('stage');
const ctx = canvas.getContext('2d');

let W = 0, H = 0;
function resize() {
    W = canvas.width = canvas.clientWidth;
    H = canvas.height = canvas.clientHeight;
}
window.addEventListener('resize', resize);
resize();

const ball = { x: 0, y: 0, r: 28, dragging: false };
ball.x = W / 2;
ball.y = H / 2;

const trail = []; // {x, y, age}
const TRAIL_LIFE = 60;

canvas.addEventListener('mousedown', (e) => {
    const dx = e.offsetX - ball.x, dy = e.offsetY - ball.y;
    if (dx * dx + dy * dy <= ball.r * ball.r) ball.dragging = true;
});
canvas.addEventListener('mousemove', (e) => {
    if (ball.dragging) {
        ball.x = e.offsetX;
        ball.y = e.offsetY;
        trail.push({ x: ball.x, y: ball.y, age: 0 });
    }
});
canvas.addEventListener('mouseup', () => { ball.dragging = false; });
canvas.addEventListener('mouseleave', () => { ball.dragging = false; });

function frame() {
    ctx.fillStyle = 'rgba(14, 16, 20, 0.4)';
    ctx.fillRect(0, 0, W, H);

    for (let i = trail.length - 1; i >= 0; i--) {
        const t = trail[i];
        t.age++;
        if (t.age > TRAIL_LIFE) { trail.splice(i, 1); continue; }
        const a = 1 - t.age / TRAIL_LIFE;
        ctx.fillStyle = `rgba(108, 140, 255, ${a * 0.4})`;
        ctx.beginPath();
        ctx.arc(t.x, t.y, ball.r * a, 0, Math.PI * 2);
        ctx.fill();
    }

    const grad = ctx.createRadialGradient(ball.x - 8, ball.y - 8, 4, ball.x, ball.y, ball.r);
    grad.addColorStop(0, '#a8bdff');
    grad.addColorStop(1, '#4a6cd0');
    ctx.fillStyle = grad;
    ctx.beginPath();
    ctx.arc(ball.x, ball.y, ball.r, 0, Math.PI * 2);
    ctx.fill();

    requestAnimationFrame(frame);
}
requestAnimationFrame(frame);
