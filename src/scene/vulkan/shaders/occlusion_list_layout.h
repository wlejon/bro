// The occlusion retest's list (SceneTargets::occlusionList), for C++ and GLSL
// alike: the forest cards the opaque batches held back because the last
// frame's pyramid hid them (scene_occlusion.glsl), which PassHiZ tests
// against this frame's and gathers, those this one does not hide, into an
// indirect draw a sprite draw for the retest (PassOpaque, retest). A slot is
// a sprite draw of the batches (OpaqueBatches::Draw::slot), carried to its
// vertex stage in push.extra.w over 65536 (slot + 1 times it). Every offset
// counts 32-bit words from the start of the buffer.
//
//   OL_TOTAL     the cards appended (atomic)
//   OL_COUNT     of them, those the test reads (OL_TOTAL, at most OL_CAP)
//   OL_DISPATCH  the test's dispatch (x, y, z), written by the prefix stage
//   OL_ENABLED   a word a slot, nonzero where its cards may be held back:
//                the slots in order while their cards fit in OL_CAP, so
//                every card a batch holds back has its entry
//   OL_HELD      a word a slot: its cards appended (atomic)
//   OL_CMDS      a VkDrawIndirectCommand a slot: one card's vertices, the
//                slot's cards this frame's pyramid does not hide, from its
//                first in OL_SORTED (as the first instance)
//   OL_ENTRIES   OL_ENTRY_WORDS a card: its slot, instance index, first
//                vertex, nearest depth (float bits) and its pixel rect's
//                corners (x | y << 16), OL_SHOWN where the test must not
//                hide it (a corner off the depth range or behind the eye)
//   OL_SORTED    two words a card the retest draws: instance index, first
//                vertex; each slot's from its OL_CMDS first instance
//
// The budget stage runs at the batches' start (PassOcclusionBudget) and
// clears OL_TOTAL and OL_HELD, counting each slot's cards from its draw
// (OlDraw: the CPU's counts, or the GPU planner's command, read where the
// planner wrote it); the prefix and test stages run after the pyramid
// (PassHiZ).
//
// The budget's draws (a uniform block, OL_SLOTS of them): x, y the
// vertices and instances of a CPU draw; z OL_DRAW_CPU, a planner's buffer
// (0 to OL_PLANNED - 1, a binding each) whose command is at word w, or
// OL_DRAW_NONE (a slot nothing holds back).

#ifndef BRO_SCENE_OCCLUSION_LIST_LAYOUT_H
#define BRO_SCENE_OCCLUSION_LIST_LAYOUT_H

#define OL_SLOTS        255    // (slot + 1) * 65536 stays exact in a float
#define OL_CAP          (1 << 19)
#define OL_ENTRY_WORDS  6
#define OL_SHOWN        0xffffffffu
#define OL_GROUP_SIZE   256    // at least OL_SLOTS: the budget and prefix take a thread a slot

#define OL_STAGE_BUDGET 0
#define OL_STAGE_PREFIX 1
#define OL_STAGE_TEST   2

#define OL_PLANNED      4      // the planners' buffers a budget reads
#define OL_DRAW_CPU     0xfffffffeu
#define OL_DRAW_NONE    0xffffffffu

#define OL_TOTAL        0
#define OL_COUNT        1
#define OL_DISPATCH     4
#define OL_ENABLED      8
#define OL_HELD         (OL_ENABLED + OL_SLOTS)
#define OL_CMDS         (OL_HELD + OL_SLOTS)
#define OL_ENTRIES      (OL_CMDS + 4 * OL_SLOTS)
#define OL_SORTED       (OL_ENTRIES + OL_ENTRY_WORDS * OL_CAP)
#define OL_WORDS        (OL_SORTED + 2 * OL_CAP)

#endif
