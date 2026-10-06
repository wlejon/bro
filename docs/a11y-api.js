/**
 * @file docs/a11y-api.js
 * @summary Documentation and examples for the `bro.a11y` JavaScript API.
 */

/**
 * `bro.a11y` provides accessibility tree inspection, node query/mutation, screen reader announcements,
 * and custom-role widget hooks:
 * - Live-region announcements (`announce(text, { priority: "polite" | "assertive" })`)
 * - Accessibility tree queries (`getRootNode`, `getNode`, `getFocusedNode`, `hitTest`, `findNodesByRole`)
 * - Tree transactions (`beginTransaction`, `commitTransaction`, `rollbackTransaction`)
 * - Custom role registrations (`registerCustomRole`, `getCustomRoles`)
 * - Canvas / custom widget hooks (`registerWidgetHook`, `unregisterWidgetHook`)
 * - Accessibility event subscriptions (`on`, `off`, `addEventListener`, `removeEventListener`)
 *
 * General API: Mounted in Bronze when `BRO_WITH_A11Y` is enabled; available to all applications.
 */

// ============================================================================
// 1. Screen Reader Announcements
// ============================================================================

if (bro.a11y.available) {
    // Polite announcement (queued after current speech completes)
    bro.a11y.announce('File download completed', { priority: 'polite' });

    // Assertive announcement (interrupts current speech for urgent alerts)
    // bro.a11y.announce('Connection lost', { priority: 'assertive' });
}

// ============================================================================
// 2. Tree Inspection & Navigation
// ============================================================================

if (bro.a11y.available) {
    const root = bro.a11y.getRootNode();
    if (root) {
        console.log(`Root accessible node #${root.id}: role="${root.role}", name="${root.name}"`);
        console.log(`Children count: ${root.childIds.length}`);
    }

    const focused = bro.a11y.getFocusedNode();
    if (focused) {
        console.log(`Focused node: #${focused.id} (${focused.role})`);
    }
}

// ============================================================================
// 3. Custom Roles & Canvas Widget Hooks
// ============================================================================

if (bro.a11y.available) {
    // Register custom accessible widget role
    bro.a11y.registerCustomRole('colorPicker', {
        baseRole: 'slider',
        description: 'Interactive color selection palette',
    });

    // Subscribe to announcements
    const sub = bro.a11y.on('announce', (event) => {
        console.log(`Accessibility announcement: "${event.text}" (priority: ${event.priority})`);
    });

    // Unsubscribe when finished
    // sub.remove();
}
