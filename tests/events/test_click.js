// Test click event dispatching via addEventListener

const root = document.getElementById('root');

// Create a button-like element with known dimensions
root.innerHTML = '<div id="btn" style="width:100px;height:50px;position:absolute;left:0;top:0;">Click me</div>';
flush();

let clicked = false;
let clickEvent = null;
const btn = document.getElementById('btn');
btn.addEventListener('click', (e) => {
    clicked = true;
    clickEvent = e;
});

// Simulate click in the middle of the button
click(50, 25);

assert(clicked, 'click handler was called');
assert(clickEvent !== null, 'event object received');
assert(clickEvent.type === 'click', 'event type is click');
assert(clickEvent.target === btn, 'event target is the button');

// Cleanup
root.innerHTML = '';
