// Skeleton visualization - stub. Future: bone spheres + connector cylinders.
window.SkeletonViz = { build(scene, skeleton, pose) { return []; }, destroy(nodes) { for (const n of (nodes||[])) { try { n.destroy(); } catch(_) {} } } };
