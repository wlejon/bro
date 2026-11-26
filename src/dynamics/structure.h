// dynamics::Structure: something built (a tower, a bridge, a wall) standing in a World, which
// anything with enough behind it breaks.
//
// It is given as members, boxes with a mass and what their material holds, and the ties
// between members that touch. Each member is cut into cells about `chunk` across, joined
// to their neighbours across the faces they share: the structure is that graph. While it
// stands it has no bodies. A member that something moving comes near is given a fixed body
// for as long as the thing is there, so it is met as a wall is.
//
// A joint lets go once the work done on it passes what it holds over the distance it gives.
// What meets a standing cell with more than its joints take goes through: the cell comes
// loose (a body of its own from then on), its neighbours after it while the blow has more
// to give, and the thing is slowed by what that cost it. With less, the cell keeps the
// damage and the thing stops against it. What comes loose in one piece is one body of many
// boxes, which breaks the same way where it lands or is struck. A cell pressed harder than
// its material bears, standing or loose, is crushed: it is gone, and named in crushed().
//
// After damage, and while anything rests on it, the weight of what still stands and of what
// rests there is sent down the joints again to the ground. A joint holds what its section
// does (pressed, pulled or sheared), or `safety` times what it carried as built if that is
// more: a cell asked to bear more is crushed out, a joint asked to hang or shear more lets
// go. Cells further sideways from what holds them up than their member spans let go too.
// The weight is sent again a moment later, so a collapse runs on in time. Cells left with
// no way to the ground fall.
#pragma once

#include "dynamics/world.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace bro::dynamics {

struct MemberDesc {
    std::uint32_t frame = 0;   // which of the structure's frames its box is in
    float c[3] = {0, 0, 0};    // centre, in the frame
    float h[3] = {0.5f, 0.5f, 0.5f};
    float mass = 1000.0f;      // kg
    float sigma = 3.0e6f;      // Pa: what its material holds pulled or sheared
    float crush = 3.0e7f;      // Pa: pressed
    float give = 0.02f;        // m: how far a joint of it stretches before it lets go
    float span = 5.0f;        // m: how far sideways from what holds it up it reaches
    bool anchored = false;     // held by the ground
};

struct TieDesc {
    std::uint32_t a = 0, b = 0;  // two members that touch
};

struct StructureDesc {
    std::vector<Pose> frames;
    std::vector<MemberDesc> members;
    std::vector<TieDesc> ties;
    float chunk = 1.2f;    // m: the size of the cells
    float safety = 2.0f;   // a joint holds this many times what it carried as built
    float friction = 0.7f;
    float restitution = 0.0f;
    std::uint32_t layers = 1;
    std::uint32_t mask = 0xffffffffu;
};

// A cell as built.
struct CellInfo {
    std::uint32_t member = 0;
    Pose pose;
    float h[3] = {0, 0, 0};
    float mass = 0;
};

// Where a cell that is not standing is now.
struct CellMove {
    std::uint32_t cell = 0;
    Pose pose;
};

// Cells that came loose together: one body.
struct PieceInfo {
    std::uint32_t body = 0;
    int cells = 0;
    float mass = 0;
};

struct StructureCounts {
    int cells = 0;
    int standing = 0;   // cells that have not come loose
    int pieces = 0;     // loose bodies
    int held = 0;       // members with a fixed body now
    int joints = 0;     // joints that hold
    double solveMs = 0; // the last sending of the weight
};

inline constexpr std::int32_t kStanding = -1;
inline constexpr std::int32_t kGone = -2;

class Structure {
public:
    Structure(World& world, std::uint32_t index, const StructureDesc& d);
    ~Structure();
    Structure(const Structure&) = delete;
    Structure& operator=(const Structure&) = delete;

    const std::vector<CellInfo>& cells() const;
    // Per cell: kStanding, kGone, or the body it is part of.
    void state(std::vector<std::int32_t>& out) const;
    // The loose cells the last step moved or made.
    const std::vector<CellMove>& moved() const;
    // The cells the last step crushed: they are gone.
    const std::vector<std::uint32_t>& crushed() const;
    void pieces(std::vector<PieceInfo>& out) const;
    // The cells of a loose body; empty when it is not one of this structure's.
    const std::vector<std::uint32_t>& pieceCells(std::uint32_t body) const;
    // Takes a loose body out of the world; its cells are gone.
    void drop(std::uint32_t body);
    StructureCounts counts() const;
    // The nearest standing cell along a ray (dir need not be unit): `body` is the cell.
    // Loose cells are bodies, which the world's raycast finds.
    RayHit raycast(const float origin[3], const float dir[3], float maxDistance) const;

    // The world's side of it. before: the bodies that are moving, ahead of a step. meet:
    // a body of this structure about to be touched at a point; false lets the other
    // through for this step. after: once the step is done.
    void before(float dt, const std::vector<Awake>& awake);
    bool meet(std::uint32_t mine, std::uint32_t other, const float point[3], const float normal[3]);
    void after(float dt);
    // Takes every body of it out of the world.
    void clear();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace bro::dynamics
