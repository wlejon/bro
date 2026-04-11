// Test getComputedStyle

const root = document.getElementById('root');
root.innerHTML = '<div id="styled" style="width:200px;height:100px;color:rgb(255,0,0);">text</div>';
flush();

const el = document.getElementById('styled');
const cs = getComputedStyle(el);

// width and height should reflect the inline style
assert(cs.width === '200px', 'computed width: ' + cs.width);
assert(cs.height === '100px', 'computed height: ' + cs.height);

// getPropertyValue
assert(cs.getPropertyValue('width') === '200px', 'getPropertyValue width');

// Cleanup
root.innerHTML = '';
