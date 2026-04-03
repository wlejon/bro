// Register shadow-container custom element.
// Its shadow DOM has overflow:hidden on :host and a <slot> for light DOM children.
// This mimics the game-ui pattern where the host clips but inner content scrolls.
customElements.define('shadow-container', class extends HTMLElement {
    constructor() {
        super();
        var shadow = this.attachShadow({ mode: 'open' });
        shadow.innerHTML =
            '<style>' +
            '  :host { display: block; overflow: hidden; }' +
            '  .wrapper { padding: 8px; }' +
            '</style>' +
            '<div class="wrapper"><slot></slot></div>';
    }
});
