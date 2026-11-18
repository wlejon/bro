/**
 * =============================================================================
 * customElements & HTMLElement — Web Components Custom Elements Registry
 * =============================================================================
 *
 * Implements the W3C Custom Elements specification: custom element registration,
 * constructor invocation, lifecycle callbacks (connectedCallback,
 * disconnectedCallback, attributeChangedCallback), and DOM subtree upgrading.
 *
 * @example
 *   // Define and register a custom element
 *   class MyCounter extends HTMLElement {
 *     count: number = 0;
 *     static get observedAttributes() { return ["count"]; }
 *     constructor() {
 *       super();
 *       this.count = 0;
 *     }
 *     connectedCallback() {
 *       console.log("Connected to DOM");
 *     }
 *     disconnectedCallback() {
 *       console.log("Disconnected from DOM");
 *     }
 *     attributeChangedCallback(name, oldValue, newValue) {
 *       console.log("Attribute " + name + " changed from " + oldValue + " to " + newValue);
 *     }
 *   }
 *   customElements.define("my-counter", MyCounter);
 *
 * @example
 *   // Instantiate custom element via document.createElement
 *   const el = document.createElement("my-counter");
 *   document.body.appendChild(el);
 *
 * Shadow roots (`attachShadow`) are EventTargets on the composed path: a
 * listener on the root hears events from inside its tree (currentTarget is
 * the root; a non-composed event stops there, a composed one continues to
 * the host retargeted), and `shadowRoot.dispatchEvent` targets the root. A
 * <slot> whose assigned nodes change (children added or removed, a child's
 * `slot` or the slot's `name` changed) gets a bubbling `slotchange` at the
 * next microtask checkpoint.
 *
 * @example
 *   const root = el.attachShadow({ mode: "open" });
 *   root.innerHTML = "<slot></slot>";
 *   root.addEventListener("slotchange", (e) => console.log(e.target.assignedElements()));
 */

// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * Options for custom element definition (e.g. customized built-in element extension).
 * @typedef {Object} CustomElementOptions
 * @property {string} [extends] -  Tag name of built-in element being extended.
 */

// ── Classes & Interfaces ─────────────────────────────────────────────────────

/**
 * Custom element registry for registering and querying custom element definitions.
 */
class CustomElementRegistry {

  /**
   * Register a new custom element definition.
   *
   * @param {string} name - Custom element tag name (must contain a hyphen)
   * @param {Function} constructor - Class constructor extending HTMLElement
   */
  define(name, constructor) {}

  /**
   * Retrieve constructor for a registered custom element.
   *
   * @param {string} name - Custom element tag name
   * @returns {Function|undefined} Constructor function or undefined
   */
  get(name) {}

  /**
   * Return a promise that resolves with the constructor when the named custom element is defined.
   *
   * @param {string} name - Custom element tag name
   * @returns {Promise<Function>}
   */
  whenDefined(name) {}

  /**
   * Upgrade all custom elements within a DOM subtree.
   *
   * @param {Node} root - Root node of the subtree to upgrade
   */
  upgrade(root) {}

}

/**
 * Base class for all HTML elements, extended by custom web components.
 */
class HTMLElement {

  /**
   * Creates a new HTMLElement instance.
   */
  constructor() {}

}

