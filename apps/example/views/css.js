window.views = window.views || {};
window.views.css = {
    init: function(el) {
        var cards = el.querySelectorAll('.css-toggle');
        for (var i = 0; i < cards.length; i++) {
            cards[i].addEventListener('click', function() {
                var box = this.querySelector('.box');
                if (box) box.classList.toggle('active');
            });
        }
    }
};
