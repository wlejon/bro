#include "dynamics/structure.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <queue>
#include <unordered_map>
#include <utility>

namespace bro::dynamics {

namespace {

struct V3 {
    float x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float length(V3 a) { return std::sqrt(dot(a, a)); }
V3 from(const float* p) { return {p[0], p[1], p[2]}; }
void to(float* out, V3 v) {
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
}
float axisOf(V3 v, int k) { return k == 0 ? v.x : k == 1 ? v.y : v.z; }

struct Q {
    float x = 0, y = 0, z = 0, w = 1;
};
Q fromQ(const float* q) { return {q[0], q[1], q[2], q[3]}; }
Q conj(Q q) { return {-q.x, -q.y, -q.z, q.w}; }
Q mul(Q a, Q b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
V3 rot(Q q, V3 v) {
    const V3 u{q.x, q.y, q.z};
    const V3 t = cross(u, v) * 2.0f;
    return v + t * q.w + cross(u, t);
}

constexpr float kInf = std::numeric_limits<float>::infinity();
constexpr float kTouch = 0.05f;   // m: members this near are joined, and the least a joint is across
// A way to the ground is as long as it goes sideways: up and down are all but free, so what
// hangs is shared by every floor it is tied to, and ways within kEven of the shortest share.
constexpr float kUpright = 0.001f;
constexpr float kEven = 0.05f;
constexpr float kMeetSpeed = 1.0f;  // m/s: slower than this is leaning, not a blow
constexpr float kReach = 3.0f;    // m: the furthest one blow breaks from where it lands
constexpr float kSettle = 0.12f;  // s between one sending of the weight and the next
constexpr int kPasses = 8;        // times a loose body's push is sent again after something in it gave
constexpr std::size_t kBeam = 48; // cells: a loose body of fewer is too small to bend as a beam
constexpr float kLanding = 0.05f; // of what a loose body loses where it lands, the part that breaks it
constexpr int kHold = 30;      // steps a member keeps its body after the last thing near it
constexpr float kGridCell = 8.0f;

struct Cell {
    V3 p;                 // centre as built, world
    float h[3] = {0, 0, 0};
    std::uint32_t member = 0;
    float mass = 0;
    float damage = 0;     // J its joints have taken from blows that did not free it
    float crush = 0;      // Pa it bears
    float span = 5;       // m it may be from the ground sideways
    float far0 = 0;       // m it was, as built
    float pressed = 0;    // N s of what rests on it, since the weight was last sent
    std::int32_t piece = kStanding;  // kStanding, kGone, or its piece
    int struck = -1000;   // the step of the last blow counted
    std::uint32_t adj0 = 0, adjN = 0;
    bool anchored = false;
};

struct Joint {
    std::uint32_t a = 0, b = 0;
    float area = 0;       // of the face they share
    float sigma = 0, crush = 0;
    float energy = 0;     // J to part it
    float weight = 0;     // what a path through it costs
    float aside = 0;      // m between the two centres, level
    float p0 = 0;         // N through it as built
    std::int8_t to0 = -1; // the end that bore it as built: 0 a, 1 b
    bool upright = false; // one cell over the other
    bool aUnder = false;
    bool held = true;
};

struct Member {
    V3 c;                 // centre, world
    float h[3] = {0, 0, 0};
    Q q;
    V3 min, max;
    float mass = 0;
    std::uint32_t cell0 = 0;
    int cells = 0;
    int standing = 0;
    std::uint32_t body = kWorld;  // its fixed body while something is near
    int until = 0;
    std::uint32_t seen = 0;
    bool stale = false;   // its body shows cells that have left
};

struct Piece {
    std::uint32_t body = kWorld;
    std::vector<std::uint32_t> cells;  // ascending
    V3 origin;            // where the body's origin is among the cells as built
    float mass = 0;
    bool fresh = false;
    bool alive = false;
};

struct Blow {
    std::uint32_t member = 0, other = 0;
    V3 point;
    float energy = 0;
};

std::uint64_t gridKey(int x, int y, int z) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x) & 0x1fffffu) << 42) |
           (static_cast<std::uint64_t>(static_cast<std::uint32_t>(y) & 0x1fffffu) << 21) |
           static_cast<std::uint64_t>(static_cast<std::uint32_t>(z) & 0x1fffffu);
}

}  // namespace

struct Structure::Impl {
    World& world;
    std::uint32_t index;
    float chunk, safety, friction, restitution;
    std::uint32_t layers, mask;
    V3 up{0, 1, 0};
    float g = 9.81f;

    std::vector<Cell> cells;
    std::vector<CellInfo> info;
    std::vector<Joint> joints;
    std::vector<std::uint32_t> adj;  // per cell, the joints on it
    std::vector<Member> members;
    std::vector<Piece> pieces;
    std::vector<std::uint32_t> freePieces;
    std::vector<std::int32_t> bodyPiece, bodyMember;  // by body
    std::vector<std::uint32_t> held;                  // members with a body
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> grid;
    V3 lo{kInf, kInf, kInf}, hi{-kInf, -kInf, -kInf};
    std::vector<Blow> blows;
    std::vector<CellMove> moved;
    std::vector<std::uint32_t> gone;  // the cells the last step crushed
    std::vector<std::uint32_t> none;
    int tick = 0;
    int standing = 0, jointsHeld = 0, pieceCount = 0;
    bool unsettled = false;
    float pressedOver = 0;  // s over which the cells' `pressed` was gathered
    double solveMs = 0;
    std::uint32_t stamp = 0;

    // Scratch.
    std::vector<float> dist, load;
    std::vector<std::uint64_t> block;
    std::vector<V3> push;  // N s on each loose cell, in the space it was built in, since squeeze
    std::vector<std::uint32_t> order, found, mark;
    std::vector<std::pair<float, std::uint32_t>> near;

    Impl(World& w, std::uint32_t i, const StructureDesc& d)
        : world(w), index(i), chunk(d.chunk > 0.05f ? d.chunk : 0.05f), safety(d.safety > 1.0f ? d.safety : 1.0f),
          friction(d.friction), restitution(d.restitution), layers(d.layers), mask(d.mask) {
        float gv[3];
        world.gravity(gv);
        g = length(from(gv));
        if (g > 1e-6f) up = from(gv) * (-1.0f / g);
        build(d);
    }

    V3 axis(const Member& m, int k) const { return rot(m.q, V3{k == 0 ? 1.0f : 0.0f, k == 1 ? 1.0f : 0.0f, k == 2 ? 1.0f : 0.0f}); }

    // A joint between cells i and j, whose faces overlap by ou and ov.
    void join(std::uint32_t i, std::uint32_t j, float ou, float ov, const MemberDesc& da, const MemberDesc& db) {
        ou = std::max(ou, kTouch);
        ov = std::max(ov, kTouch);
        Joint e;
        e.a = i;
        e.b = j;
        e.area = ou * ov;
        e.sigma = std::min(da.sigma, db.sigma);
        e.crush = std::min(da.crush, db.crush);
        e.energy = e.sigma * e.area * std::min(da.give, db.give);
        const V3 d = cells[j].p - cells[i].p;
        const float rise = dot(d, up);
        e.aside = length(d - up * rise);
        e.upright = std::fabs(rise) >= e.aside;
        e.aUnder = rise > 0.0f;
        e.weight = std::max(kUpright * std::fabs(rise) + e.aside, 1.0e-4f);
        joints.push_back(e);
    }

    void build(const StructureDesc& d) {
        members.resize(d.members.size());
        for (std::size_t mi = 0; mi < d.members.size(); mi++) {
            const MemberDesc& md = d.members[mi];
            const Pose frame = md.frame < d.frames.size() ? d.frames[md.frame] : Pose{};
            Member& m = members[mi];
            m.q = fromQ(frame.q);
            m.c = from(frame.p) + rot(m.q, from(md.c));
            m.mass = md.mass;
            int n[3];
            float ch[3];
            for (int k = 0; k < 3; k++) {
                m.h[k] = std::max(md.h[k], 0.005f);
                n[k] = std::max(1, static_cast<int>(std::floor(2.0f * m.h[k] / chunk + 0.5f)));
                ch[k] = m.h[k] / static_cast<float>(n[k]);
            }
            m.cell0 = static_cast<std::uint32_t>(cells.size());
            m.cells = n[0] * n[1] * n[2];
            m.standing = m.cells;
            const auto at = [&](int x, int y, int z) {
                return m.cell0 + static_cast<std::uint32_t>((x * n[1] + y) * n[2] + z);
            };
            for (int x = 0; x < n[0]; x++) {
                for (int y = 0; y < n[1]; y++) {
                    for (int z = 0; z < n[2]; z++) {
                        Cell c;
                        const V3 local{-m.h[0] + ch[0] * static_cast<float>(2 * x + 1), -m.h[1] + ch[1] * static_cast<float>(2 * y + 1),
                                       -m.h[2] + ch[2] * static_cast<float>(2 * z + 1)};
                        c.p = m.c + rot(m.q, local);
                        for (int k = 0; k < 3; k++) c.h[k] = ch[k];
                        c.member = static_cast<std::uint32_t>(mi);
                        c.mass = md.mass / static_cast<float>(m.cells);
                        c.anchored = md.anchored;
                        c.span = md.span;
                        c.crush = md.crush;
                        cells.push_back(c);
                    }
                }
            }
            for (int x = 0; x < n[0]; x++) {
                for (int y = 0; y < n[1]; y++) {
                    for (int z = 0; z < n[2]; z++) {
                        if (x + 1 < n[0]) join(at(x, y, z), at(x + 1, y, z), 2 * ch[1], 2 * ch[2], md, md);
                        if (y + 1 < n[1]) join(at(x, y, z), at(x, y + 1, z), 2 * ch[2], 2 * ch[0], md, md);
                        if (z + 1 < n[2]) join(at(x, y, z), at(x, y, z + 1), 2 * ch[0], 2 * ch[1], md, md);
                    }
                }
            }
            // The box about it in the world.
            V3 e{0, 0, 0};
            for (int k = 0; k < 3; k++) {
                const V3 a = axis(m, k);
                e = e + V3{std::fabs(a.x), std::fabs(a.y), std::fabs(a.z)} * m.h[k];
            }
            m.min = m.c - e;
            m.max = m.c + e;
            lo = {std::min(lo.x, m.min.x), std::min(lo.y, m.min.y), std::min(lo.z, m.min.z)};
            hi = {std::max(hi.x, m.max.x), std::max(hi.y, m.max.y), std::max(hi.z, m.max.z)};
            eachGrid(m.min, m.max, [&](std::vector<std::uint32_t>& slot) { slot.push_back(static_cast<std::uint32_t>(mi)); }, true);
        }

        for (const TieDesc& t : d.ties) {
            if (t.a >= members.size() || t.b >= members.size() || t.a == t.b) continue;
            tie(t.a, t.b, d.members[t.a], d.members[t.b]);
        }

        // The joints on each cell.
        for (const Joint& e : joints) {
            cells[e.a].adjN++;
            cells[e.b].adjN++;
        }
        std::uint32_t at = 0;
        for (Cell& c : cells) {
            c.adj0 = at;
            at += c.adjN;
            c.adjN = 0;
        }
        adj.resize(at);
        for (std::size_t j = 0; j < joints.size(); j++) {
            for (std::uint32_t c : {joints[j].a, joints[j].b}) adj[cells[c].adj0 + cells[c].adjN++] = static_cast<std::uint32_t>(j);
        }

        standing = static_cast<int>(cells.size());
        jointsHeld = static_cast<int>(joints.size());
        mark.assign(cells.size(), 0);
        info.resize(cells.size());
        for (std::size_t i = 0; i < cells.size(); i++) {
            const Cell& c = cells[i];
            const Member& m = members[c.member];
            CellInfo& o = info[i];
            o.member = c.member;
            to(o.pose.p, c.p);
            o.pose.q[0] = m.q.x;
            o.pose.q[1] = m.q.y;
            o.pose.q[2] = m.q.z;
            o.pose.q[3] = m.q.w;
            for (int k = 0; k < 3; k++) o.h[k] = c.h[k];
            o.mass = c.mass;
        }
        weigh(true);
        release();
    }

    // The cells of two members that touch, joined.
    void tie(std::uint32_t ma, std::uint32_t mb, const MemberDesc& da, const MemberDesc& db) {
        const Member& A = members[ma];
        const Member& B = members[mb];
        // B's axes as A sees them.
        float r[3][3];
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) r[i][j] = std::fabs(dot(axis(A, i), axis(B, j)));
        }
        const Q toA = conj(A.q);
        bool any = false;
        float best = kInf;
        std::uint32_t bi = 0, bj = 0;
        for (int i = 0; i < A.cells; i++) {
            const Cell& ca = cells[A.cell0 + static_cast<std::uint32_t>(i)];
            for (int j = 0; j < B.cells; j++) {
                const Cell& cb = cells[B.cell0 + static_cast<std::uint32_t>(j)];
                const V3 d = rot(toA, cb.p - ca.p);
                float o[3];
                bool touch = true;
                for (int k = 0; k < 3; k++) {
                    const float hb = r[k][0] * cb.h[0] + r[k][1] * cb.h[1] + r[k][2] * cb.h[2];
                    o[k] = ca.h[k] + hb - std::fabs(axisOf(d, k));
                    touch = touch && o[k] >= -kTouch;
                }
                const float gap = length(d);
                if (gap < best) {
                    best = gap;
                    bi = A.cell0 + static_cast<std::uint32_t>(i);
                    bj = B.cell0 + static_cast<std::uint32_t>(j);
                }
                if (!touch) continue;
                int k = 0;
                if (o[1] < o[k]) k = 1;
                if (o[2] < o[k]) k = 2;
                join(A.cell0 + static_cast<std::uint32_t>(i), B.cell0 + static_cast<std::uint32_t>(j), o[(k + 1) % 3],
                     o[(k + 2) % 3], da, db);
                any = true;
            }
        }
        // Tied but apart: the nearest two cells hold them together.
        if (!any && A.cells > 0 && B.cells > 0) join(bi, bj, kTouch, kTouch, da, db);
    }

    template <class F>
    void eachGrid(V3 mn, V3 mx, F&& f, bool make) {
        const auto cell = [](float v) { return static_cast<int>(std::floor(v / kGridCell)); };
        for (int x = cell(mn.x); x <= cell(mx.x); x++) {
            for (int y = cell(mn.y); y <= cell(mx.y); y++) {
                for (int z = cell(mn.z); z <= cell(mx.z); z++) {
                    if (make) {
                        f(grid[gridKey(x, y, z)]);
                    } else {
                        const auto it = grid.find(gridKey(x, y, z));
                        if (it != grid.end()) f(it->second);
                    }
                }
            }
        }
    }

    // The members with cells standing whose boxes reach into a box, each once.
    template <class F>
    void eachMember(V3 mn, V3 mx, F&& f) {
        mn = {std::max(mn.x, lo.x), std::max(mn.y, lo.y), std::max(mn.z, lo.z)};
        mx = {std::min(mx.x, hi.x), std::min(mx.y, hi.y), std::min(mx.z, hi.z)};
        if (mn.x > mx.x || mn.y > mx.y || mn.z > mx.z) return;
        stamp++;
        eachGrid(mn, mx, [&](std::vector<std::uint32_t>& slot) {
            for (std::uint32_t mi : slot) {
                Member& m = members[mi];
                if (m.seen == stamp) continue;
                m.seen = stamp;
                if (m.standing < 1) continue;
                if (m.min.x > mx.x || m.max.x < mn.x || m.min.y > mx.y || m.max.y < mn.y || m.min.z > mx.z || m.max.z < mn.z) continue;
                f(mi);
            }
        }, false);
    }

    std::uint32_t across(const Joint& e, std::uint32_t c) const { return e.a == c ? e.b : e.a; }

    // What it takes to part a cell from everything it is joined to.
    float cost(std::uint32_t c) const {
        const Cell& cell = cells[c];
        float sum = 0;
        for (std::uint32_t k = 0; k < cell.adjN; k++) {
            const Joint& e = joints[adj[cell.adj0 + k]];
            if (e.held) sum += e.energy;
        }
        return std::max(sum - cell.damage, 0.0f);
    }

    void part(std::uint32_t c) {
        const Cell& cell = cells[c];
        for (std::uint32_t k = 0; k < cell.adjN; k++) {
            Joint& e = joints[adj[cell.adj0 + k]];
            if (!e.held) continue;
            e.held = false;
            jointsHeld--;
        }
    }

    void leave(std::uint32_t c) {
        Member& m = members[cells[c].member];
        m.standing--;
        m.stale = true;
        standing--;
    }

    // ---- bodies ---------------------------------------------------------------------

    void note(std::vector<std::int32_t>& map, std::uint32_t body, std::int32_t value) {
        if (body >= map.size()) map.resize(static_cast<std::size_t>(body) + 1, -1);
        map[body] = value;
    }
    std::int32_t pieceOf(std::uint32_t body) const { return body < bodyPiece.size() ? bodyPiece[body] : -1; }
    std::int32_t memberOf(std::uint32_t body) const { return body < bodyMember.size() ? bodyMember[body] : -1; }

    BodyDesc bodyDesc() const {
        BodyDesc d;
        d.friction = friction;
        d.restitution = restitution;
        d.layers = layers;
        d.mask = mask;
        return d;
    }

    void unhold(Member& m) {
        if (m.body == kWorld) return;
        note(bodyMember, m.body, -1);
        world.removeBody(m.body);
        m.body = kWorld;
    }

    // A member's fixed body: its box while it is whole, else the cells of it that stand.
    void hold(std::uint32_t mi) {
        Member& m = members[mi];
        std::vector<Part> parts;
        if (m.standing == m.cells) {
            Part p;
            for (int k = 0; k < 3; k++) p.h[k] = m.h[k];
            parts.push_back(p);
        } else {
            const Q in = conj(m.q);
            for (int i = 0; i < m.cells; i++) {
                const Cell& c = cells[m.cell0 + static_cast<std::uint32_t>(i)];
                if (c.piece != kStanding) continue;
                Part p;
                to(p.c, rot(in, c.p - m.c));
                for (int k = 0; k < 3; k++) p.h[k] = c.h[k];
                parts.push_back(p);
            }
        }
        m.stale = false;
        if (parts.empty()) return;
        BodyDesc d = bodyDesc();
        to(d.pose.p, m.c);
        d.pose.q[0] = m.q.x;
        d.pose.q[1] = m.q.y;
        d.pose.q[2] = m.q.z;
        d.pose.q[3] = m.q.w;
        d.structure = index;
        m.body = world.addBoxes(d, parts.data(), static_cast<int>(parts.size()));
        note(bodyMember, m.body, static_cast<std::int32_t>(mi));
    }

    // Cells (ascending) as one loose body. With `old`, they were part of that body, which
    // is where they are and how they move; else they are where they were built, at rest.
    void loose(std::vector<std::uint32_t>&& list, const Pose* old, V3 oldOrigin, std::uint32_t oldBody, V3 push = {}) {
        if (list.empty()) return;
        std::uint32_t pi;
        if (!freePieces.empty()) {
            pi = freePieces.back();
            freePieces.pop_back();
        } else {
            pi = static_cast<std::uint32_t>(pieces.size());
            pieces.emplace_back();
        }
        V3 sum{0, 0, 0};
        float mass = 0;
        for (std::uint32_t c : list) {
            sum = sum + cells[c].p * cells[c].mass;
            mass += cells[c].mass;
        }
        const V3 origin = mass > 0 ? sum * (1.0f / mass) : cells[list[0]].p;
        std::vector<Part> parts;
        parts.reserve(list.size());
        for (std::size_t i = 0; i < list.size();) {
            const Cell& c = cells[list[i]];
            const Member& m = members[c.member];
            Part p;
            p.q[0] = m.q.x;
            p.q[1] = m.q.y;
            p.q[2] = m.q.z;
            p.q[3] = m.q.w;
            // A whole member is one box.
            const std::size_t n = static_cast<std::size_t>(m.cells);
            if (n > 1 && list[i] == m.cell0 && i + n <= list.size() && list[i + n - 1] == m.cell0 + n - 1) {
                to(p.c, m.c - origin);
                for (int k = 0; k < 3; k++) p.h[k] = m.h[k];
                p.mass = m.mass;
                i += n;
            } else {
                to(p.c, c.p - origin);
                for (int k = 0; k < 3; k++) p.h[k] = c.h[k];
                p.mass = c.mass;
                i++;
            }
            parts.push_back(p);
        }
        BodyDesc d = bodyDesc();
        if (old) {
            const Q q = fromQ(old->q);
            const V3 at = from(old->p) + rot(q, origin - oldOrigin);
            to(d.pose.p, at);
            for (int k = 0; k < 4; k++) d.pose.q[k] = old->q[k];
            float lin[3], ang[3], atp[3];
            to(atp, at);
            world.velocity(oldBody, lin, ang);
            world.velocityAt(oldBody, atp, d.velocity);
            for (int k = 0; k < 3; k++) d.angular[k] = ang[k];
        } else {
            to(d.pose.p, origin);
            to(d.velocity, push);
        }
        Piece& p = pieces[pi];
        p.body = world.addBoxes(d, parts.data(), static_cast<int>(parts.size()));
        p.origin = origin;
        p.mass = mass;
        p.fresh = true;
        p.alive = true;
        for (std::uint32_t c : list) cells[c].piece = static_cast<std::int32_t>(pi);
        p.cells = std::move(list);
        note(bodyPiece, p.body, static_cast<std::int32_t>(pi));
        pieceCount++;
    }

    void retire(std::uint32_t pi, bool gone) {
        Piece& p = pieces[pi];
        if (!p.alive) return;
        if (gone) {
            for (std::uint32_t c : p.cells) {
                part(c);
                cells[c].piece = kGone;
            }
        }
        note(bodyPiece, p.body, -1);
        world.removeBody(p.body);
        p.body = kWorld;
        p.cells.clear();
        p.alive = false;
        freePieces.push_back(pi);
        pieceCount--;
    }

    // ---- what stands ----------------------------------------------------------------

    // Groups of `list` cells (marked with `tag` in mark) still joined to each other.
    template <class F>
    void groups(const std::vector<std::uint32_t>& list, std::uint32_t tag, F&& f) {
        std::vector<std::uint32_t> stack, group;
        for (std::uint32_t start : list) {
            if (mark[start] != tag) continue;
            group.clear();
            stack.push_back(start);
            mark[start] = 0;
            while (!stack.empty()) {
                const std::uint32_t c = stack.back();
                stack.pop_back();
                group.push_back(c);
                const Cell& cell = cells[c];
                for (std::uint32_t k = 0; k < cell.adjN; k++) {
                    const Joint& e = joints[adj[cell.adj0 + k]];
                    if (!e.held) continue;
                    const std::uint32_t o = across(e, c);
                    if (mark[o] != tag) continue;
                    mark[o] = 0;
                    stack.push_back(o);
                }
            }
            std::sort(group.begin(), group.end());
            f(std::vector<std::uint32_t>(group));
        }
    }

    // The standing cells with no joints left to the ground come loose, each group as one
    // body.
    void release() {
        const std::uint32_t tag = ++stamp;
        found.clear();
        for (std::uint32_t i = 0; i < cells.size(); i++) {
            if (cells[i].piece == kStanding && cells[i].anchored) {
                mark[i] = tag;
                found.push_back(i);
            }
        }
        for (std::size_t at = 0; at < found.size(); at++) {
            const Cell& cell = cells[found[at]];
            for (std::uint32_t k = 0; k < cell.adjN; k++) {
                const Joint& e = joints[adj[cell.adj0 + k]];
                if (!e.held) continue;
                const std::uint32_t o = across(e, found[at]);
                if (mark[o] == tag || cells[o].piece != kStanding) continue;
                mark[o] = tag;
                found.push_back(o);
            }
        }
        if (static_cast<int>(found.size()) == standing) return;
        const std::uint32_t adrift = ++stamp;
        std::vector<std::uint32_t> list;
        for (std::uint32_t i = 0; i < cells.size(); i++) {
            if (cells[i].piece != kStanding || mark[i] == tag) continue;
            mark[i] = adrift;
            list.push_back(i);
        }
        for (std::uint32_t c : list) leave(c);
        groups(list, adrift, [&](std::vector<std::uint32_t>&& group) { loose(std::move(group), nullptr, V3{}, kWorld); });
    }

    // What a joint bears pressed: it was made for `safety` times what it carried as built,
    // and for no less than its material holds pulled.
    float bears(const Joint& e) const { return std::max(e.sigma * e.area, safety * e.p0); }

    // How far sideways from the ground a cell may be.
    float limit(const Cell& c) const { return std::max(c.span, 1.25f * c.far0 + 0.5f); }

    // Sends the weight of what stands, and of what rests on it, to the ground: each cell's
    // goes down the joints on its shortest ways there, shared between them. Cells further
    // from the ground sideways than their member spans let go of the cells that are not; a
    // joint asked to hang or shear more than it holds lets go; a cell asked to bear more
    // than it holds is crushed out. `record` keeps what each joint carries and how far each
    // cell is as what they were as built, and breaks nothing. Returns whether anything gave.
    bool weigh(bool record) {
        const auto t0 = std::chrono::steady_clock::now();
        const std::size_t n = cells.size();
        dist.assign(n, kInf);
        load.assign(n, 0.0f);
        using Entry = std::pair<float, std::uint32_t>;
        std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> heap;
        const float over = pressedOver > 0 ? 1.0f / pressedOver : 0.0f;
        for (std::uint32_t i = 0; i < n; i++) {
            Cell& cell = cells[i];
            if (cell.piece == kStanding) {
                load[i] = cell.mass * g + cell.pressed * over;
                if (cell.anchored) {
                    dist[i] = 0.0f;
                    heap.push({0.0f, i});
                }
            }
            cell.pressed = 0;
        }
        pressedOver = 0;
        order.clear();
        while (!heap.empty()) {
            const Entry top = heap.top();
            heap.pop();
            const std::uint32_t c = top.second;
            if (top.first > dist[c]) continue;
            order.push_back(c);
            const Cell& cell = cells[c];
            for (std::uint32_t k = 0; k < cell.adjN; k++) {
                const Joint& e = joints[adj[cell.adj0 + k]];
                if (!e.held) continue;
                const std::uint32_t o = across(e, c);
                if (cells[o].piece != kStanding) continue;
                const float d = top.first + e.weight;
                if (d < dist[o]) {
                    dist[o] = d;
                    heap.push({d, o});
                }
            }
        }
        if (record) {
            for (std::uint32_t c : order) cells[c].far0 = dist[c];
        }
        std::vector<std::uint32_t> cut, crushed;
        // Too far out: what is, lets go of what is not, and falls in one piece.
        const std::uint32_t out = ++stamp;
        if (!record) {
            for (std::uint32_t c : order) {
                if (dist[c] > limit(cells[c])) mark[c] = out;
            }
            for (std::uint32_t c : order) {
                if (mark[c] != out) continue;
                const Cell& cell = cells[c];
                for (std::uint32_t k = 0; k < cell.adjN; k++) {
                    const std::uint32_t ji = adj[cell.adj0 + k];
                    if (joints[ji].held && mark[across(joints[ji], c)] != out) cut.push_back(ji);
                }
            }
        }
        // Furthest from the ground first, so a cell has all it is sent before it sends.
        for (std::size_t at = order.size(); at-- > 0;) {
            const std::uint32_t c = order[at];
            const Cell& cell = cells[c];
            if (cell.anchored) continue;
            int ways = 0;
            const float reach = dist[c] + kEven;
            for (std::uint32_t k = 0; k < cell.adjN; k++) {
                const Joint& e = joints[adj[cell.adj0 + k]];
                if (!e.held) continue;
                const std::uint32_t o = across(e, c);
                if (dist[o] < dist[c] && dist[o] + e.weight <= reach) ways++;
            }
            if (ways < 1) continue;
            const float p = load[c] / static_cast<float>(ways);
            for (std::uint32_t k = 0; k < cell.adjN; k++) {
                const std::uint32_t ji = adj[cell.adj0 + k];
                Joint& e = joints[ji];
                if (!e.held) continue;
                const std::uint32_t o = across(e, c);
                if (!(dist[o] < dist[c] && dist[o] + e.weight <= reach)) continue;
                load[o] += p;
                const std::int8_t to = o == e.a ? 0 : 1;
                if (record) {
                    e.p0 = p;
                    e.to0 = to;
                    continue;
                }
                const bool under = e.upright && (to == 0) == e.aUnder;  // what bears it is beneath
                float cap = e.sigma * e.area;
                if (to == e.to0) cap = std::max(cap, safety * e.p0);
                if (under) cap = bears(e);
                if (p <= cap) continue;
                if (under && !cells[o].anchored) {
                    crushed.push_back(o);
                } else {
                    cut.push_back(ji);
                }
            }
        }
        bool gave = false;
        for (std::uint32_t ji : cut) {
            if (!joints[ji].held) continue;
            joints[ji].held = false;
            jointsHeld--;
            gave = true;
        }
        std::sort(crushed.begin(), crushed.end());
        crushed.erase(std::unique(crushed.begin(), crushed.end()), crushed.end());
        for (std::uint32_t c : crushed) {
            part(c);
            leave(c);
            cells[c].piece = kGone;
            gone.push_back(c);
            gave = true;
        }
        solveMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return gave;
    }

    // What rests on the members that have bodies, kept for the next sending of the weight.
    void pressing(float dt) {
        std::vector<Press> list;
        bool any = false;
        for (std::uint32_t mi : held) {
            const Member& m = members[mi];
            if (m.body == kWorld || world.touching(m.body) == 0) continue;
            world.pressing(m.body, list);
            for (const Press& p : list) {
                const float down = -dot(from(p.impulse), up);
                if (!(down > 0)) continue;
                const V3 at = from(p.point);
                std::uint32_t nearest = kWorld;
                float best = kInf;
                for (int i = 0; i < m.cells; i++) {
                    const std::uint32_t c = m.cell0 + static_cast<std::uint32_t>(i);
                    if (cells[c].piece != kStanding) continue;
                    const float d = length(cells[c].p - at);
                    if (d < best) {
                        best = d;
                        nearest = c;
                    }
                }
                if (nearest == kWorld) continue;
                cells[nearest].pressed += down;
                any = true;
            }
        }
        if (any) unsettled = true;
        // And on the loose bodies, whichever way it pushes.
        if (push.size() != cells.size()) push.assign(cells.size(), V3{});
        for (const Piece& p : pieces) {
            if (!p.alive || world.touching(p.body) == 0) continue;
            world.pressing(p.body, list);
            if (list.empty()) continue;
            const Pose pose = world.pose(p.body);
            const Q back = conj(fromQ(pose.q));
            for (const Press& pr : list) {
                const V3 at = p.origin + rot(back, from(pr.point) - from(pose.p));
                std::uint32_t nearest = p.cells[0];
                float best = kInf;
                for (std::uint32_t c : p.cells) {
                    const float d = length(cells[c].p - at);
                    if (d < best) {
                        best = d;
                        nearest = c;
                    }
                }
                cells[nearest].pressed += length(from(pr.impulse));
                push[nearest] = push[nearest] + rot(back, from(pr.impulse));
            }
        }
    }


    // ---- blows ----------------------------------------------------------------------

    float reachOf(std::uint32_t other) const {
        float mn[3], mx[3];
        world.bounds(other, mn, mx);
        const float half = 0.5f * std::min(mx[0] - mn[0], std::min(mx[1] - mn[1], mx[2] - mn[2]));
        return std::min(std::max(half, 0.5f * chunk), kReach) + 0.5f * chunk;
    }

    // Frees the standing cell nearest a point and then the others in a box (the room the
    // thing that struck takes up), nearest first, while the blow has it to spend. Returns
    // what was spent.
    float strike(V3 point, float budget, V3 mn, V3 mx, std::vector<std::uint32_t>& freed) {
        near.clear();
        const V3 r{chunk, chunk, chunk};
        eachMember(mn - r, mx + r, [&](std::uint32_t mi) {
            const Member& m = members[mi];
            for (int i = 0; i < m.cells; i++) {
                const std::uint32_t c = m.cell0 + static_cast<std::uint32_t>(i);
                if (cells[c].piece == kStanding && !cells[c].anchored) near.push_back({length(cells[c].p - point), c});
            }
        });
        std::sort(near.begin(), near.end());
        float spent = 0;
        bool first = true;
        for (const auto& [d, c] : near) {
            // Any of the cell in the box, by more than a skin.
            const V3 p = cells[c].p;
            const Member& m = members[cells[c].member];
            V3 e{-0.02f, -0.02f, -0.02f};
            for (int k = 0; k < 3; k++) {
                const V3 a = axis(m, k);
                e = e + V3{std::fabs(a.x), std::fabs(a.y), std::fabs(a.z)} * cells[c].h[k];
            }
            const bool inside = p.x + e.x >= mn.x && p.x - e.x <= mx.x && p.y + e.y >= mn.y && p.y - e.y <= mx.y &&
                                p.z + e.z >= mn.z && p.z - e.z <= mx.z;
            if (!inside && !first) continue;
            first = false;
            // What it cannot free is left standing in its way, to stop it.
            const float need = cost(c);
            if (budget < need) break;
            budget -= need;
            spent += need;
            part(c);
            leave(c);
            cells[c].piece = kGone;  // until it is given its body
            freed.push_back(c);
        }
        return spent;
    }

    void strikes(float dt) {
        if (blows.empty()) return;
        std::vector<std::uint32_t> freed;
        bool any = false;
        for (const Blow& b : blows) {
            if (!world.hasBody(b.other)) continue;
            float lin[3], ang[3], mn[3], mx[3];
            world.velocity(b.other, lin, ang);
            world.bounds(b.other, mn, mx);
            V3 v = from(lin);
            const float mass = world.mass(b.other);
            const float had = mass > 0 ? 0.5f * mass * dot(v, v) : kInf;
            const float budget = std::min(b.energy, had);
            if (!(budget > 0)) continue;
            // The room it takes up now and over the step to come.
            const V3 travel = v * dt;
            const V3 lo{mn[0] + std::min(travel.x, 0.0f), mn[1] + std::min(travel.y, 0.0f), mn[2] + std::min(travel.z, 0.0f)};
            const V3 hi{mx[0] + std::max(travel.x, 0.0f), mx[1] + std::max(travel.y, 0.0f), mx[2] + std::max(travel.z, 0.0f)};
            freed.clear();
            const float used = strike(b.point, budget, lo, hi, freed);
            // What it broke cost the thing that broke it, and what came loose it takes
            // along: they go on together.
            float carried = 0;
            for (std::uint32_t c : freed) carried += cells[c].mass;
            if (mass > 0) {
                const float keep = had > used ? std::sqrt(1.0f - used / had) : 0.0f;
                v = v * (keep * mass / (mass + carried));
                to(lin, v);
                world.setVelocity(b.other, lin, ang);
            }
            std::sort(freed.begin(), freed.end());
            for (std::uint32_t c : freed) loose(std::vector<std::uint32_t>{c}, nullptr, V3{}, kWorld, v);
            any = any || !freed.empty();
        }
        blows.clear();
        if (!any) return;
        release();
        unsettled = true;
    }

    // A loose body of several cells, struck: cells about the point come away one by one
    // while the blow has it to spend, and with more to spend the body cracks further out,
    // into blocks twice as far across for each doubling of how far they are from the point.
    // What is left goes on as the bodies it is now.
    void shatter(std::uint32_t pi, V3 point, float budget, float reach) {
        Piece& p = pieces[pi];
        const Pose pose = world.pose(p.body);
        const V3 at = p.origin + rot(conj(fromQ(pose.q)), point - from(pose.p));
        near.clear();
        for (std::uint32_t c : p.cells) near.push_back({length(cells[c].p - at), c});
        std::sort(near.begin(), near.end());
        std::vector<std::uint32_t> freed;
        const std::uint32_t tag = ++stamp;
        std::size_t rest = 0;
        for (; rest < near.size(); rest++) {
            const std::uint32_t c = near[rest].second;
            if (near[rest].first > reach && !freed.empty()) break;
            const float need = cost(c);
            if (budget < need) {
                cells[c].damage += budget;
                budget = 0;
                break;
            }
            budget -= need;
            part(c);
            freed.push_back(c);
            mark[c] = tag;
            if (freed.size() + 1 >= p.cells.size()) {
                rest++;
                break;
            }
        }
        if (budget > 0 && reach > 0) {
            if (block.size() < cells.size()) block.resize(cells.size());
            for (std::size_t i = rest; i < near.size(); i++) {
                const std::uint32_t c = near[i].second;
                const int band = std::min(static_cast<int>(std::log2(std::max(near[i].first, reach) / reach)), 8);
                const float across = 2.0f * chunk * static_cast<float>(1 << band);
                const V3 r = (cells[c].p - at) * (1.0f / across);
                block[c] = gridKey(static_cast<int>(std::floor(r.x)), static_cast<int>(std::floor(r.y)), static_cast<int>(std::floor(r.z))) ^
                           (static_cast<std::uint64_t>(band) * 0x9E3779B97F4A7C15ull);
            }
            const std::int32_t own = cells[p.cells[0]].piece;
            for (std::size_t i = rest; i < near.size() && budget > 0; i++) {
                const std::uint32_t c = near[i].second;
                const Cell& cell = cells[c];
                for (std::uint32_t k = 0; k < cell.adjN; k++) {
                    Joint& e = joints[adj[cell.adj0 + k]];
                    if (!e.held) continue;
                    const std::uint32_t o = this->across(e, c);
                    if (cells[o].piece != own || mark[o] == tag || block[o] == block[c]) continue;
                    if (budget < e.energy) {
                        budget = 0;
                        break;
                    }
                    budget -= e.energy;
                    e.held = false;
                    jointsHeld--;
                }
            }
        }
        split(pi, freed, false);
    }

    // Takes cells (already parted from their neighbours) out of a loose body, each as a body
    // of its own or, crushed, as nothing; what is left of it goes on as the bodies it is now.
    void split(std::uint32_t pi, std::vector<std::uint32_t>& out, bool crushed) {
        Piece& p = pieces[pi];
        const Pose pose = world.pose(p.body);
        const std::uint32_t tag = ++stamp;
        for (std::uint32_t c : p.cells) mark[c] = tag;
        for (std::uint32_t c : out) mark[c] = 0;
        const std::vector<std::uint32_t> rest = std::move(p.cells);
        const V3 origin = p.origin;
        const std::uint32_t body = p.body;
        // The new bodies are made from the old one's motion before it goes.
        std::vector<std::vector<std::uint32_t>> made;
        std::sort(out.begin(), out.end());
        for (std::uint32_t c : out) {
            if (crushed) {
                cells[c].piece = kGone;
                gone.push_back(c);
            } else {
                made.push_back({c});
            }
        }
        groups(rest, tag, [&](std::vector<std::uint32_t>&& group) { made.push_back(std::move(group)); });
        if (out.empty() && made.size() == 1) {  // still one body
            p.cells = rest;
            return;
        }
        p.cells.clear();
        note(bodyPiece, body, -1);
        p.alive = false;
        pieceCount--;
        for (auto& list : made) loose(std::move(list), &pose, origin, body);
        world.removeBody(body);
        pieces[pi].body = kWorld;
        freePieces.push_back(pi);
    }

    // A loose body is pushed where it touches, and every cell of it has to be moved by that
    // push: it goes from the cells touched out through the joints, as the weight of what
    // stands goes to the ground. A cell asked to push more than it bears is crushed, and a
    // joint asked to pull or shear more than it holds lets go. `force` is the push on the
    // whole body, in the space it was built in. Returns whether a joint let go.
    bool strain(std::uint32_t pi, V3 force, std::vector<std::uint32_t>& out) {
        const Piece& p = pieces[pi];
        if (p.cells.size() < 4) return false;
        const float f = length(force);
        if (!(f > 0.5f * p.mass * g)) return false;
        const V3 dir = force * (1.0f / f);
        const float a = f / p.mass;
        if (dist.size() < cells.size()) dist.resize(cells.size());
        if (load.size() < cells.size()) load.resize(cells.size());
        using Entry = std::pair<float, std::uint32_t>;
        std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> heap;
        for (std::uint32_t c : p.cells) {
            load[c] = cells[c].mass * a;
            dist[c] = kInf;
            if (dot(push[c], push[c]) > 0) {
                dist[c] = 0.0f;
                heap.push({0.0f, c});
            }
        }
        const std::int32_t own = cells[p.cells[0]].piece;
        order.clear();
        while (!heap.empty()) {
            const Entry top = heap.top();
            heap.pop();
            const std::uint32_t c = top.second;
            if (top.first > dist[c]) continue;
            order.push_back(c);
            const Cell& cell = cells[c];
            for (std::uint32_t k = 0; k < cell.adjN; k++) {
                const Joint& e = joints[adj[cell.adj0 + k]];
                if (!e.held) continue;
                const std::uint32_t o = across(e, c);
                if (cells[o].piece != own) continue;
                const float d = top.first + length(cells[o].p - cell.p);
                if (d < dist[o]) {
                    dist[o] = d;
                    heap.push({d, o});
                }
            }
        }
        std::vector<std::uint32_t> cut;
        const auto way = [&](const Joint& e, std::uint32_t c, std::uint32_t o) {
            return e.held && cells[o].piece == own && dist[o] < dist[c] &&
                   dist[o] + length(cells[o].p - cells[c].p) <= dist[c] + kEven;
        };
        for (std::size_t at = order.size(); at-- > 0;) {
            const std::uint32_t c = order[at];
            const Cell& cell = cells[c];
            if (!(dist[c] > 0)) continue;
            int ways = 0;
            for (std::uint32_t k = 0; k < cell.adjN; k++) {
                const Joint& e = joints[adj[cell.adj0 + k]];
                if (way(e, c, across(e, c))) ways++;
            }
            if (ways < 1) continue;
            const float share = load[c] / static_cast<float>(ways);
            for (std::uint32_t k = 0; k < cell.adjN; k++) {
                const std::uint32_t ji = adj[cell.adj0 + k];
                const Joint& e = joints[ji];
                const std::uint32_t o = across(e, c);
                if (!way(e, c, o)) continue;
                load[o] += share;
                const V3 d = cell.p - cells[o].p;
                const bool pushes = dot(d, dir) > 0.7f * length(d);  // o is behind c, the way the push goes
                if (share <= (pushes ? bears(e) : e.sigma * e.area)) continue;
                if (pushes) {
                    out.push_back(o);
                } else {
                    cut.push_back(ji);
                }
            }
        }
        for (std::uint32_t ji : cut) {
            if (!joints[ji].held) continue;
            joints[ji].held = false;
            jointsHeld--;
        }
        return !cut.empty();
    }

    // A loose body pushed where it touches is bent by it: across each plane of its grid the
    // part to one side is held to the rest by the joints through that plane, which share
    // the pull, the shear and the turning between them as a beam's section does. A joint
    // asked for more than its material holds lets go, and the others take what it carried.
    // `force` is the push on the whole body, in the space it was built in.
    bool bend(std::uint32_t pi, V3 force, float window) {
        const Piece& p = pieces[pi];
        if (p.cells.size() < kBeam) return false;
        if (!(length(force) > 0.5f * p.mass * g)) return false;
        const V3 acc = force * (1.0f / p.mass);
        const std::int32_t own = cells[p.cells[0]].piece;
        const float over = 1.0f / window;
        struct Side {
            float m = 0;
            V3 mp, f, t;
        };
        std::vector<Side> sides;
        std::vector<std::vector<std::uint32_t>> crossing;
        bool any = false;
        for (int k = 0; k < 3; k++) {
            const int u = (k + 1) % 3, v = (k + 2) % 3;
            float lo = kInf, hi = -kInf;
            for (std::uint32_t c : p.cells) {
                lo = std::min(lo, axisOf(cells[c].p, k));
                hi = std::max(hi, axisOf(cells[c].p, k));
            }
            const int layers = static_cast<int>(std::floor((hi - lo) / chunk + 0.5f)) + 1;
            if (layers < 2) continue;
            const auto layerOf = [&](std::uint32_t c) {
                return std::clamp(static_cast<int>(std::floor((axisOf(cells[c].p, k) - lo) / chunk + 0.5f)), 0, layers - 1);
            };
            sides.assign(static_cast<std::size_t>(layers), Side{});
            crossing.assign(static_cast<std::size_t>(layers), {});
            for (std::uint32_t c : p.cells) {
                const Cell& cell = cells[c];
                const int l = layerOf(c);
                Side& s = sides[static_cast<std::size_t>(l)];
                const V3 f = push[c] * over;
                s.m += cell.mass;
                s.mp = s.mp + cell.p * cell.mass;
                s.f = s.f + f;
                s.t = s.t + cross(cell.p, f);
                for (std::uint32_t j = 0; j < cell.adjN; j++) {
                    const std::uint32_t ji = adj[cell.adj0 + j];
                    const Joint& e = joints[ji];
                    const std::uint32_t o = across(e, c);
                    if (!e.held || o < c || cells[o].piece != own) continue;
                    const int lo2 = layerOf(o);
                    if (lo2 != l) crossing[static_cast<std::size_t>(std::min(l, lo2))].push_back(ji);
                }
            }
            Side sum;
            for (int l = 0; l + 1 < layers; l++) {
                const Side& s = sides[static_cast<std::size_t>(l)];
                sum.m += s.m;
                sum.mp = sum.mp + s.mp;
                sum.f = sum.f + s.f;
                sum.t = sum.t + s.t;
                const std::vector<std::uint32_t>& through = crossing[static_cast<std::size_t>(l)];
                if (through.empty() || !(sum.m > 0)) continue;
                for (int pass = 0; pass < 3; pass++) {
                    float area = 0;
                    V3 r0{};
                    for (std::uint32_t ji : through) {
                        const Joint& e = joints[ji];
                        if (!e.held) continue;
                        area += e.area;
                        r0 = r0 + (cells[e.a].p + cells[e.b].p) * (0.5f * e.area);
                    }
                    if (!(area > 0)) break;
                    r0 = r0 * (1.0f / area);
                    // What has to come through the plane for the near side to move as the body does.
                    const V3 need = acc * sum.m - sum.f;
                    const V3 turn = cross(sum.mp * (1.0f / sum.m) - r0, acc * sum.m) - (sum.t - cross(r0, sum.f));
                    float iu = 0, iv = 0;
                    for (std::uint32_t ji : through) {
                        const Joint& e = joints[ji];
                        if (!e.held) continue;
                        const V3 d = (cells[e.a].p + cells[e.b].p) * 0.5f - r0;
                        iu += e.area * (axisOf(d, u) * axisOf(d, u) + e.area / 12.0f);
                        iv += e.area * (axisOf(d, v) * axisOf(d, v) + e.area / 12.0f);
                    }
                    const float pull = axisOf(need, k) / area;
                    const float shear = std::sqrt(axisOf(need, u) * axisOf(need, u) + axisOf(need, v) * axisOf(need, v)) / area;
                    const float bv = axisOf(turn, u) / iv, bu = -axisOf(turn, v) / iu;
                    bool let = false;
                    for (std::uint32_t ji : through) {
                        Joint& e = joints[ji];
                        if (!e.held) continue;
                        const V3 d = (cells[e.a].p + cells[e.b].p) * 0.5f - r0;
                        const float edge = 0.5f * std::sqrt(e.area) * (std::fabs(bv) + std::fabs(bu));
                        const float stress = std::max(pull + bv * axisOf(d, v) + bu * axisOf(d, u) + edge, 0.0f) + shear;
                        if (stress <= e.sigma) continue;
                        e.held = false;
                        jointsHeld--;
                        let = true;
                    }
                    if (!let) break;
                    any = true;
                }
            }
        }
        return any;
    }

    // The cells of loose bodies pressed harder than their material bears are crushed, and
    // the bodies come apart where the push through them is more than they hold.
    void squeeze(float window) {
        if (!(window > 0)) return;
        if (push.size() != cells.size()) push.assign(cells.size(), V3{});
        std::vector<std::uint32_t> out;
        for (std::uint32_t pi = 0; pi < pieces.size(); pi++) {
            if (!pieces[pi].alive) continue;
            out.clear();
            V3 sum{};
            for (std::uint32_t c : pieces[pi].cells) sum = sum + push[c];
            // What gives sends what it carried another way, and that may give in turn.
            bool cut = bend(pi, sum * (1.0f / window), window);
            for (int pass = 0; pass < kPasses; pass++) {
                const std::size_t had = out.size();
                const bool let = strain(pi, sum * (1.0f / window), out);
                cut = cut || let;
                for (std::size_t i = had; i < out.size(); i++) part(out[i]);
                if (!let && out.size() == had) break;
            }
            for (std::uint32_t c : pieces[pi].cells) {
                push[c] = V3{};
                Cell& cell = cells[c];
                if (cell.pressed > 0) {
                    // What goes in one side comes out the other: half of what touched it.
                    const float force = 0.5f * cell.pressed / window;
                    float h0 = cell.h[0], h1 = cell.h[1], h2 = cell.h[2];
                    if (h0 < h1) std::swap(h0, h1);
                    if (h1 < h2) std::swap(h1, h2);
                    if (h0 < h1) std::swap(h0, h1);
                    if (force > cell.crush * 4.0f * h0 * h1) out.push_back(c);
                }
                cell.pressed = 0;
            }
            if (out.empty() && !cut) continue;
            std::sort(out.begin(), out.end());
            out.erase(std::unique(out.begin(), out.end()), out.end());
            for (std::uint32_t c : out) part(c);
            split(pi, out, true);
        }
    }

    void landings() {
        struct Landing {
            std::uint32_t piece;
            V3 point;
            float energy, reach;
        };
        std::vector<Landing> list;
        for (const Hit& h : world.hits()) {
            for (int side = 0; side < 2; side++) {
                const std::uint32_t mine = side ? h.b : h.a, other = side ? h.a : h.b;
                const std::int32_t pi = pieceOf(mine);
                if (pi < 0 || pieces[static_cast<std::size_t>(pi)].cells.size() < 2) continue;
                const float m1 = pieces[static_cast<std::size_t>(pi)].mass;
                const float m2 = world.hasBody(other) ? world.mass(other) : 0.0f;
                const float mu = m2 > 0 ? m1 * m2 / (m1 + m2) : m1;
                // Half of what the meeting loses goes into each of the two, and most of that
                // into heat and dust, not into parting joints.
                const float energy = kLanding * 0.25f * mu * h.speed * h.speed;
                auto it = std::find_if(list.begin(), list.end(), [&](const Landing& l) { return l.piece == static_cast<std::uint32_t>(pi); });
                if (it == list.end()) {
                    list.push_back({static_cast<std::uint32_t>(pi), from(h.point), energy, reachOf(other)});
                } else if (energy > it->energy) {
                    it->point = from(h.point);
                    it->energy = energy;
                    it->reach = reachOf(other);
                }
            }
        }
        for (const Landing& l : list) shatter(l.piece, l.point, l.energy, l.reach);
    }

    void report() {
        moved.clear();
        const auto add = [&](const Piece& p, const Pose& pose) {
            const Q q = fromQ(pose.q);
            for (std::uint32_t c : p.cells) {
                CellMove mv;
                mv.cell = c;
                to(mv.pose.p, from(pose.p) + rot(q, cells[c].p - p.origin));
                const Q turn = mul(q, members[cells[c].member].q);
                mv.pose.q[0] = turn.x;
                mv.pose.q[1] = turn.y;
                mv.pose.q[2] = turn.z;
                mv.pose.q[3] = turn.w;
                moved.push_back(mv);
            }
        };
        for (const Moved& m : world.moved()) {
            const std::int32_t pi = pieceOf(m.body);
            if (pi < 0 || pieces[static_cast<std::size_t>(pi)].fresh) continue;
            add(pieces[static_cast<std::size_t>(pi)], m.pose);
        }
        for (Piece& p : pieces) {
            if (!p.alive || !p.fresh) continue;
            p.fresh = false;
            add(p, world.pose(p.body));
        }
    }
};

Structure::Structure(World& world, std::uint32_t index, const StructureDesc& d) : impl_(std::make_unique<Impl>(world, index, d)) {}
Structure::~Structure() = default;

const std::vector<CellInfo>& Structure::cells() const { return impl_->info; }

void Structure::state(std::vector<std::int32_t>& out) const {
    out.resize(impl_->cells.size());
    for (std::size_t i = 0; i < out.size(); i++) {
        const std::int32_t p = impl_->cells[i].piece;
        out[i] = p < 0 ? p : static_cast<std::int32_t>(impl_->pieces[static_cast<std::size_t>(p)].body);
    }
}

const std::vector<CellMove>& Structure::moved() const { return impl_->moved; }
const std::vector<std::uint32_t>& Structure::crushed() const { return impl_->gone; }

void Structure::pieces(std::vector<PieceInfo>& out) const {
    out.clear();
    for (const Piece& p : impl_->pieces) {
        if (!p.alive) continue;
        PieceInfo i;
        i.body = p.body;
        i.cells = static_cast<int>(p.cells.size());
        i.mass = p.mass;
        out.push_back(i);
    }
}

const std::vector<std::uint32_t>& Structure::pieceCells(std::uint32_t body) const {
    const std::int32_t pi = impl_->pieceOf(body);
    return pi < 0 ? impl_->none : impl_->pieces[static_cast<std::size_t>(pi)].cells;
}

void Structure::drop(std::uint32_t body) {
    const std::int32_t pi = impl_->pieceOf(body);
    if (pi >= 0) impl_->retire(static_cast<std::uint32_t>(pi), true);
}

RayHit Structure::raycast(const float origin[3], const float dir[3], float maxDistance) const {
    const Impl& s = *impl_;
    RayHit out;
    const float len = length(from(dir));
    if (!(len > 1e-9f) || !(maxDistance > 0.0f)) return out;
    const V3 o = from(origin), d = from(dir) * (1.0f / len);
    float best = maxDistance;
    // A ray against a box in a member's frame: the distance in, and the face it enters by.
    const auto box = [](V3 lo, V3 ld, V3 c, const float* h, float& t, int& face) {
        float t0 = 0.0f, t1 = kInf;
        face = -1;
        for (int k = 0; k < 3; k++) {
            const float p = axisOf(lo, k) - axisOf(c, k), v = axisOf(ld, k);
            if (std::fabs(v) < 1e-9f) {
                if (std::fabs(p) > h[k]) return false;
                continue;
            }
            float a = (-h[k] - p) / v, b = (h[k] - p) / v;
            if (a > b) std::swap(a, b);
            if (a > t0) {
                t0 = a;
                face = k;
            }
            t1 = std::min(t1, b);
            if (t0 > t1) return false;
        }
        t = t0;
        return true;
    };
    for (const Member& m : s.members) {
        if (m.standing < 1) continue;
        const Q in = conj(m.q);
        const V3 lo = rot(in, o - m.c), ld = rot(in, d);
        float t;
        int face;
        if (!box(lo, ld, V3{}, m.h, t, face) || t >= best) continue;
        const auto take = [&](std::uint32_t cell, float at, int f) {
            best = at;
            out.hit = true;
            out.body = cell;
            out.distance = at;
            to(out.point, o + d * at);
            V3 n{0, 0, 0};
            if (f >= 0) {
                n = s.axis(m, f);
                if (dot(n, d) > 0) n = n * -1.0f;
            } else {
                n = d * -1.0f;
            }
            to(out.normal, n);
        };
        for (int i = 0; i < m.cells; i++) {
            const std::uint32_t c = m.cell0 + static_cast<std::uint32_t>(i);
            if (s.cells[c].piece != kStanding) continue;
            float tc;
            int fc;
            if (box(lo, ld, rot(in, s.cells[c].p - m.c), s.cells[c].h, tc, fc) && tc < best) take(c, tc, fc);
        }
    }
    return out;
}

StructureCounts Structure::counts() const {
    StructureCounts c;
    c.cells = static_cast<int>(impl_->cells.size());
    c.standing = impl_->standing;
    c.pieces = impl_->pieceCount;
    c.held = static_cast<int>(impl_->held.size());
    c.joints = impl_->jointsHeld;
    c.solveMs = impl_->solveMs;
    return c;
}

void Structure::before(float dt, const std::vector<Awake>& awake) {
    Impl& s = *impl_;
    s.tick++;
    for (const Awake& a : awake) {
        if (s.memberOf(a.body) >= 0) continue;
        V3 mn = from(a.min), mx = from(a.max);
        // Where it will be in two steps, and half a metre about that.
        const V3 travel = from(a.velocity) * (2.0f * dt);
        mn = mn + V3{std::min(travel.x, 0.0f) - 0.5f, std::min(travel.y, 0.0f) - 0.5f, std::min(travel.z, 0.0f) - 0.5f};
        mx = mx + V3{std::max(travel.x, 0.0f) + 0.5f, std::max(travel.y, 0.0f) + 0.5f, std::max(travel.z, 0.0f) + 0.5f};
        s.eachMember(mn, mx, [&](std::uint32_t mi) {
            Member& m = s.members[mi];
            m.until = s.tick + kHold;
            if (m.body != kWorld) return;
            s.hold(mi);
            if (m.body != kWorld) s.held.push_back(mi);
        });
    }
    for (std::size_t i = 0; i < s.held.size();) {
        Member& m = s.members[s.held[i]];
        if (m.stale) {
            s.unhold(m);
            if (m.standing > 0) s.hold(s.held[i]);
        } else if (m.until < s.tick && s.world.touching(m.body) == 0) {
            s.unhold(m);
        }
        if (m.body == kWorld) {
            s.held[i] = s.held.back();
            s.held.pop_back();
        } else {
            i++;
        }
    }
}

bool Structure::meet(std::uint32_t mine, std::uint32_t other, const float point[3], const float normal[3]) {
    Impl& s = *impl_;
    const std::int32_t mi = s.memberOf(mine);
    if (mi < 0 || s.memberOf(other) >= 0) return true;
    float v[3];
    s.world.velocityAt(other, point, v);
    const float closing = std::fabs(dot(from(v), from(normal)));
    if (closing < kMeetSpeed) return true;
    const float mass = s.world.mass(other);
    const float energy = mass > 0 ? 0.5f * mass * closing * closing : kInf;
    const Member& m = s.members[static_cast<std::size_t>(mi)];
    const V3 at = from(point);
    std::uint32_t nearest = kWorld;
    float best = kInf;
    for (int i = 0; i < m.cells; i++) {
        const std::uint32_t c = m.cell0 + static_cast<std::uint32_t>(i);
        if (s.cells[c].piece != kStanding) continue;
        const float d = length(s.cells[c].p - at);
        if (d < best) {
            best = d;
            nearest = c;
        }
    }
    if (nearest == kWorld) return false;
    if (s.cells[nearest].anchored) return true;  // the ground's own
    if (energy >= s.cost(nearest)) {
        Blow b;
        b.member = static_cast<std::uint32_t>(mi);
        b.other = other;
        b.point = at;
        b.energy = energy;
        s.blows.push_back(b);
        return false;
    }
    Cell& cell = s.cells[nearest];
    if (s.tick - cell.struck > 3) cell.damage += energy;
    cell.struck = s.tick;
    return true;
}

void Structure::after(float dt) {
    Impl& s = *impl_;
    s.gone.clear();
    s.strikes(dt);
    s.landings();
    s.pressing(dt);
    s.pressedOver += dt;
    if (s.pressedOver >= kSettle) {
        s.squeeze(s.pressedOver);
        if (s.unsettled) {
            s.unsettled = s.weigh(false);
            if (s.unsettled) s.release();
        } else {
            for (Cell& c : s.cells) c.pressed = 0;
        }
        s.pressedOver = 0;
    }
    s.report();
}

void Structure::clear() {
    Impl& s = *impl_;
    for (std::uint32_t pi = 0; pi < s.pieces.size(); pi++) s.retire(pi, true);
    for (std::uint32_t mi : s.held) s.unhold(s.members[mi]);
    s.held.clear();
}

}  // namespace bro::dynamics
