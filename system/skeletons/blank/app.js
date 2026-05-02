const items = [];
const list = document.getElementById('items');
const form = document.getElementById('add-form');
const entry = document.getElementById('entry');

function render() {
    list.innerHTML = '';
    if (items.length === 0) {
        const li = document.createElement('li');
        li.className = 'empty';
        li.textContent = 'no items';
        list.appendChild(li);
        return;
    }
    items.forEach((text, i) => {
        const li = document.createElement('li');
        li.textContent = text;
        li.addEventListener('click', () => {
            items.splice(i, 1);
            render();
        });
        list.appendChild(li);
    });
}

form.addEventListener('submit', (e) => {
    e.preventDefault();
    const v = entry.value.trim();
    if (!v) return;
    items.push(v);
    entry.value = '';
    render();
});

render();
entry.focus();
