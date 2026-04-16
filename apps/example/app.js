var fs = require('fs');
var content = document.getElementById('content');
var navLinks = document.querySelectorAll('.nav-link');
var currentView = null;
var loadedScripts = {};

function loadViewHtml(name) {
    return fs.readFileSync('views/' + name + '.html', 'utf-8');
}

function showView(name) {
    if (currentView && window.views[currentView] && window.views[currentView].destroy) {
        window.views[currentView].destroy();
    }

    content.innerHTML = loadViewHtml(name);
    content.scrollTop = 0;
    currentView = name;

    for (var i = 0; i < navLinks.length; i++) {
        var link = navLinks[i];
        link.className = link.getAttribute('data-view') === name ? 'nav-link active' : 'nav-link';
    }

    if (window.views[name] && window.views[name].init) {
        window.views[name].init(content);
    }
}

for (var i = 0; i < navLinks.length; i++) {
    navLinks[i].addEventListener('click', function() {
        showView(this.getAttribute('data-view'));
    });
}

window.views = window.views || {};
showView('css');
