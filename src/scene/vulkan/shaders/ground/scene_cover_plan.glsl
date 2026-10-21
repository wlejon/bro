// The cover's plan (vulkan/ground/scene_cover_plan.h): what each of a grass
// node's rings draws this frame, an entry a blade (the grass rings) or a
// plant (the herb rings), each a cell of the ring's grid and, for a blade,
// its slot in the cell. cover_plan.comp writes the entries; a ring's draw
// takes them as its instances (its indirect command's first instance is
// where its entries start).

#ifdef COVER_PLAN
#define COVER_LIST_ACCESS writeonly
#else
#define COVER_LIST_ACCESS readonly
#endif
layout(std430, set = 3, binding = 1) COVER_LIST_ACCESS buffer CoverList {
    uvec2 entries[];
} coverList;

// A slot under 16 (GrassNode's rings have at most 14 blades a cell; a far
// element's slot is its kind, at most GrassNode::kHerbMax).
uvec2 coverPack(ivec2 c, int j) {
    return uvec2(uint(c.x), (uint(c.y) << 4) | uint(j));
}
ivec2 coverCell(uvec2 e) {
    return ivec2(int(e.x), int(e.y) >> 4);
}
int coverSlot(uvec2 e) {
    return int(e.y & 15u);
}
