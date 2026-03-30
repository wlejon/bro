var nameInput = document.getElementById('name');
var bioTextarea = document.getElementById('bio');
var colorSelect = document.getElementById('color');
var output = document.getElementById('output');

function updateOutput() {
    var name = nameInput ? nameInput.getAttribute('value') || '' : '';
    var bio = bioTextarea ? bioTextarea.getAttribute('value') || '' : '';
    var color = colorSelect ? colorSelect.getAttribute('value') || '' : '';
    output.textContent = 'Name: ' + name + ' | Bio: ' + bio + ' | Color: ' + color;
}

if (nameInput) nameInput.addEventListener('input', updateOutput);
if (bioTextarea) bioTextarea.addEventListener('input', updateOutput);
if (colorSelect) colorSelect.addEventListener('input', updateOutput);
if (colorSelect) colorSelect.addEventListener('change', updateOutput);
