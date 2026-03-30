console.log('Vue loaded, testing DOM ops Vue uses during mount...');

try {
    // Test what Vue's renderer does
    var el = document.querySelector('#app');
    console.log('#app found:', !!el);
    console.log('#app.innerHTML:', el.innerHTML);

    // Vue creates a comment node as anchor
    var comment = document.createComment('');
    console.log('comment created, nodeType:', comment.nodeType);

    // Vue does insertBefore with comment
    el.insertBefore(comment, null);
    console.log('insertBefore(comment, null) OK');

    // Vue sets innerHTML = '' to clear mount point
    el.innerHTML = '';
    console.log('innerHTML="" OK');

    // Vue creates elements
    var div = document.createElement('div');
    console.log('createElement OK');

    // Vue sets textContent
    div.textContent = 'test';
    console.log('textContent set OK');

    // Vue uses appendChild
    el.appendChild(div);
    console.log('appendChild OK');

    // Vue creates text nodes
    var text = document.createTextNode('hello');
    console.log('createTextNode OK');
    el.appendChild(text);
    console.log('appendChild textNode OK');

    // Vue checks parentNode
    console.log('parentNode:', div.parentNode === el);
    console.log('nextSibling:', typeof div.nextSibling);

    // Now try actual Vue mount
    console.log('--- Attempting Vue mount ---');

    // Reset the app div
    el.innerHTML = '<h1>{{ message }}</h1><button id="btn">Count: {{ count }}</button>';

    const { createApp, ref } = Vue;
    const app = createApp({
        setup() {
            console.log('setup called');
            const message = ref('Hello Vue!');
            const count = ref(0);
            return { message, count };
        }
    });
    console.log('createApp OK, mounting...');
    app.mount('#app');
    console.log('mount OK!');
} catch(e) {
    console.log('Error:', e.message);
    console.log('Stack:', e.stack);
}
