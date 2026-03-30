var output = document.getElementById('output');

function log(msg) {
    output.textContent = msg;
}

// Text input
var nameInput = document.getElementById('name');
if (nameInput) nameInput.addEventListener('input', function() {
    log('Name: ' + nameInput.getAttribute('value'));
});

// Number
var numInput = document.getElementById('num');
if (numInput) numInput.addEventListener('input', function() {
    log('Number: ' + numInput.getAttribute('value'));
});

// Checkboxes
var chk1 = document.getElementById('chk1');
var chk2 = document.getElementById('chk2');
if (chk1) chk1.addEventListener('change', function() {
    log('Checkbox A: ' + chk1.checked + ', B: ' + chk2.checked);
});
if (chk2) chk2.addEventListener('change', function() {
    log('Checkbox A: ' + chk1.checked + ', B: ' + chk2.checked);
});

// Range slider
var slider = document.getElementById('slider');
var sliderVal = document.getElementById('slider-val');
if (slider) slider.addEventListener('input', function() {
    var v = slider.getAttribute('value');
    if (sliderVal) sliderVal.textContent = v;
    log('Range: ' + v);
});

// Color
var clr = document.getElementById('clr');
var clrVal = document.getElementById('clr-val');
if (clr) clr.addEventListener('change', function() {
    var v = clr.getAttribute('value');
    if (clrVal) clrVal.textContent = v;
    log('Color: ' + v);
});

// Select
var sel = document.getElementById('sel');
if (sel) sel.addEventListener('change', function() {
    log('Select: ' + sel.getAttribute('value'));
});

// Buttons
var btn1 = document.getElementById('btn1');
if (btn1) btn1.addEventListener('click', function() { log('Button clicked!'); });
var btn2 = document.getElementById('btn2');
if (btn2) btn2.addEventListener('click', function() { log('Submit clicked!'); });
var btn3 = document.getElementById('btn3');
if (btn3) btn3.addEventListener('click', function() { log('Reset clicked!'); });
