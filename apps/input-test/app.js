var logEl = document.getElementById('log');
var logLines = [];
function log(msg) {
    logLines.push(msg);
    if (logLines.length > 5) logLines.shift();
    logEl.textContent = logLines.join('\n');
}

// 1. Number validation
var num = document.getElementById('num');
if (num) num.addEventListener('input', function() {
    log('Number value: ' + num.getAttribute('value'));
});

var num2 = document.getElementById('num2');
if (num2) num2.addEventListener('input', function() {
    log('Unrestricted number: ' + num2.getAttribute('value'));
});

// 2. Tab navigation — log focus changes
var fields = ['f1', 'f2', 'f3', 'f4', 'f5'];
for (var i = 0; i < fields.length; i++) {
    (function(id) {
        var el = document.getElementById(id);
        if (el) {
            el.addEventListener('focus', function() {
                log('Focused: ' + id + ' (' + el.tagName + ')');
            });
        }
    })(fields[i]);
}

// 3. Control char filter — log what gets through
var ctrl = document.getElementById('ctrl');
if (ctrl) ctrl.addEventListener('input', function() {
    var v = ctrl.getAttribute('value') || '';
    log('Text input value: "' + v + '" (len=' + v.length + ')');
});

// 4. Textarea scroll — just works via mouse wheel

// 5. Checkbox
var chk = document.getElementById('chk');
if (chk) chk.addEventListener('change', function() {
    log('Checkbox: ' + chk.checked);
});

// 6. Range
var range = document.getElementById('range');
var rangeVal = document.getElementById('range-val');
if (range) range.addEventListener('input', function() {
    var v = range.getAttribute('value');
    if (rangeVal) rangeVal.textContent = v;
    log('Range: ' + v);
});
