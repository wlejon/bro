/**
 * CSSOM: the page's style sheets as objects (src/bronze_host/js/cssom.js over
 * dom/document_cssom.cpp).
 *
 * - `document.styleSheets` / `shadowRoot.styleSheets`: a StyleSheetList of the
 *   sheets of the <style> and <link rel=stylesheet> elements, in tree order.
 * - `styleEl.sheet` / `linkEl.sheet`: the element's CSSStyleSheet (null when it
 *   has none), with `ownerNode`, `href` (absolute, for a <link>), `media`,
 *   `disabled`, `cssRules`, `insertRule(text, index = 0)`, `deleteRule(index)`,
 *   and the legacy `rules` / `addRule` / `removeRule`.
 * - Rules: CSSStyleRule (`selectorText`, `style`, `cssText`), CSSMediaRule
 *   (`media`, `conditionText`, `cssRules`, `insertRule`, `deleteRule`),
 *   CSSSupportsRule, CSSKeyframesRule (`name`, `cssRules`, `findRule`,
 *   `appendRule`, `deleteRule`) of CSSKeyframeRule (`keyText`, `style`),
 *   CSSFontFaceRule (`style`), CSSImportRule (`href`, `media`); @layer,
 *   @container and @scope blocks are CSSGroupingRules; any other at-rule is a
 *   CSSRule carrying its `cssText`. `CSSRule.STYLE_RULE` and the other type
 *   constants are there.
 * - A rule's `style` is a CSSStyleDeclaration: `getPropertyValue`,
 *   `getPropertyPriority`, `setProperty(name, value, 'important')`,
 *   `removeProperty`, `cssText`, `length` / `item(i)`, and camelCase or dashed
 *   property access (`rule.style.backgroundColor = 'red'`).
 * - Constructable sheets: `new CSSStyleSheet({media, disabled})`,
 *   `replaceSync(text)` and `replace(text)` (a Promise of the sheet; @import is
 *   dropped), adopted through `document.adoptedStyleSheets = [sheet]` or
 *   `shadowRoot.adoptedStyleSheets`; they cascade after that scope's own sheets.
 *
 * Every change restyles: insertRule, deleteRule, a rule's selectorText or
 * style, replaceSync, `disabled`, and adoptedStyleSheets rebuild the cascade
 * from the sheet's new text. An element's own text is left as written (as in
 * a browser), and a <style> whose sheet was edited through the CSSOM keeps the
 * edited rules if its text changes later. Removing a <style> or <link> takes
 * its rules with it; inserting one before another puts its rules before them.
 * Values serialize as written (`.a { color: red; }`), not normalised.
 *
 * Shadow trees are encapsulated: a document sheet's rules do not match inside
 * a shadow tree, and a shadow root's sheets (its <style> children, its
 * adoptedStyleSheets) match only inside it, plus `:host` / `::slotted`.
 *
 * @example
 * const sheet = document.querySelector('style').sheet;
 * sheet.insertRule('.warn { color: orange; }', sheet.cssRules.length);
 * sheet.cssRules[0].style.setProperty('margin', '0');
 *
 * const shared = new CSSStyleSheet();
 * shared.replaceSync(':host { display: block; } p { margin: 0; }');
 * element.attachShadow({ mode: 'open' }).adoptedStyleSheets = [shared];
 */
