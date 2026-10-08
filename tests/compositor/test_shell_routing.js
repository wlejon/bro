// The DRM shell host's routing rules (src/engine/shell_input_rules.cpp),
// asked of a document through the headless shellClaimsPointerAt /
// shellClaimsKeyboard: pointer to the shell where the hit element's ancestry
// has z-index >= 1000, keyboard to the shell while a rendered element carries
// data-shell-keyboard or a rendered text field has focus. No ids or classes
// are special.

document.body.style.margin = '0';
document.body.innerHTML = `
  <style>
    .gone { display: none; }
    div { position: absolute; }
  </style>
  <div id="wallpaper" style="left:0; top:0; width:800px; height:600px; z-index:0"></div>
  <div id="desk-icon" style="left:10px; top:300px; width:50px; height:50px; z-index:999"></div>
  <div id="bar" style="left:0; top:0; width:800px; height:30px; z-index:1000">
    <div id="bar-btn" style="left:5px; top:5px; width:20px; height:20px"></div>
  </div>
  <div id="ghost" style="left:300px; top:300px; width:100px; height:100px; z-index:2000; pointer-events:none"></div>
  <div id="menu" class="gone" data-shell-keyboard style="left:100px; top:100px; width:100px; height:100px; z-index:6000"></div>
  <div id="lock" hidden data-shell-keyboard style="left:0; top:0; width:800px; height:600px; z-index:9000"></div>
  <div id="optout" data-shell-keyboard="false" style="left:700px; top:500px; width:10px; height:10px"></div>
  <div id="invisible" data-shell-keyboard style="visibility:hidden; left:700px; top:520px; width:10px; height:10px"></div>
  <div id="wrapper" class="gone"><div id="nested" data-shell-keyboard style="left:0; top:0; width:5px; height:5px"></div></div>
  <input id="field" type="text" style="left:600px; top:400px; width:100px; height:20px">
  <input id="check" type="checkbox" style="left:600px; top:440px; width:20px; height:20px">
  <div id="editable" contenteditable style="left:600px; top:470px; width:100px; height:20px; z-index:1">text</div>
`;
flush();

// ---- pointer --------------------------------------------------------------
assert(shellClaimsPointerAt(400, 15) === true, 'z-index 1000 bar claims the pointer');
assert(shellClaimsPointerAt(15, 15) === true, 'a child inherits its ancestor bar\'s claim');
assert(shellClaimsPointerAt(400, 200) === false, 'desktop-level wallpaper falls through to clients');
assert(shellClaimsPointerAt(30, 320) === false, 'z-index 999 is still desktop level');
assert(shellClaimsPointerAt(350, 350) === false, 'pointer-events:none overlay is not hit');
assert(shellClaimsPointerAt(150, 150) === false, 'a display:none overlay claims nothing');

document.getElementById('menu').classList.remove('gone');
flush();
assert(shellClaimsPointerAt(150, 150) === true, 'a shown z-index 6000 menu claims the pointer');

// ---- keyboard -------------------------------------------------------------
document.getElementById('menu').classList.add('gone');
flush();
assert(shellClaimsKeyboard() === false, 'nothing visible claims the keyboard');

// Toggled and asked in the same turn: the rule reads current style.
document.getElementById('menu').classList.remove('gone');
assert(shellClaimsKeyboard() === true, 'a shown data-shell-keyboard element claims the keyboard');
document.getElementById('menu').classList.add('gone');
assert(shellClaimsKeyboard() === false, 'hidden again: released');

document.getElementById('lock').hidden = false;
assert(shellClaimsKeyboard() === true, 'removing the hidden attribute claims');
document.getElementById('lock').hidden = true;
assert(shellClaimsKeyboard() === false, 'the hidden attribute releases');

document.getElementById('wrapper').classList.remove('gone');
assert(shellClaimsKeyboard() === true, 'a marked element inside a shown ancestor claims');
document.getElementById('wrapper').classList.add('gone');
assert(shellClaimsKeyboard() === false, 'a display:none ancestor hides a marked descendant');

document.getElementById('invisible').style.visibility = 'visible';
assert(shellClaimsKeyboard() === true, 'visibility:visible claims');
document.getElementById('invisible').style.visibility = 'hidden';
assert(shellClaimsKeyboard() === false, 'visibility:hidden does not claim');

// Focused editables.
document.getElementById('field').focus();
assert(document.activeElement.id === 'field', 'field focused');
assert(shellClaimsKeyboard() === true, 'a focused text input claims the keyboard');
document.getElementById('field').style.display = 'none';
assert(shellClaimsKeyboard() === false, 'focus left in a hidden field does not hold the keyboard');
document.getElementById('field').style.display = '';
document.getElementById('field').blur();
assert(shellClaimsKeyboard() === false, 'blurred: released');

document.getElementById('check').focus();
assert(shellClaimsKeyboard() === false, 'a focused checkbox is not a text field');
document.getElementById('check').blur();

document.getElementById('editable').focus();
assert(shellClaimsKeyboard() === true, 'a focused contenteditable claims the keyboard');
document.getElementById('editable').blur();
assert(shellClaimsKeyboard() === false, 'and releases on blur');

console.log('test_shell_routing.js PASSED');
