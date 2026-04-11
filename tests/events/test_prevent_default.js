// Test preventDefault

const root = document.getElementById('root');
root.innerHTML = '<div id="target" style="width:100px;height:50px;position:absolute;left:0;top:0;">target</div>';
flush();

const target = document.getElementById('target');
let defaultPrevented = false;

target.addEventListener('click', (e) => {
    e.preventDefault();
    defaultPrevented = e.defaultPrevented;
});

click(50, 25);

assert(defaultPrevented === true, 'defaultPrevented is true after preventDefault()');

// Cleanup
root.innerHTML = '';
