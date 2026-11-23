#include "table/table_range_index.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace bro::table {

namespace {

bool locEq(RowLoc a, RowLoc b) {
    return a.chunkIdx == b.chunkIdx && a.rowIdx == b.rowIdx;
}

bool extractCoords(double& x, double& y, double& z, uint8_t dims) {
    if (!std::isfinite(x) || !std::isfinite(y)) return false;
    if (dims >= 3) {
        if (!std::isfinite(z)) return false;
    } else {
        z = 0.0;
    }
    return true;
}

}  // namespace

SpatialIndex::SpatialIndex(std::string name, std::vector<size_t> cols, double cellSize)
    : name_(std::move(name)),
      cellSize_(cellSize > 1e-9 ? cellSize : 16.0) {
    dims_ = (cols.size() >= 3) ? 3 : 2;
    for (size_t i = 0; i < std::min<size_t>(cols.size(), 3); ++i) {
        cols_[i] = cols[i];
    }
    clear(64);
}

std::shared_ptr<SpatialIndex> SpatialIndex::createMapped(
    std::string name, std::vector<size_t> cols, double cellSize,
    const Bucket* buckets, size_t bucketCount,
    const Node* nodes, size_t nodeCount,
    size_t usedBuckets, size_t entryCount,
    std::shared_ptr<const void> owner) {
    auto si = std::make_shared<SpatialIndex>();
    si->name_ = std::move(name);
    si->dims_ = (cols.size() >= 3) ? 3 : 2;
    for (size_t i = 0; i < std::min<size_t>(cols.size(), 3); ++i) {
        si->cols_[i] = cols[i];
    }
    si->cellSize_ = cellSize > 1e-9 ? cellSize : 16.0;
    si->mappedOwner_ = std::move(owner);
    si->mappedBuckets_ = buckets;
    si->mappedBucketCount_ = bucketCount;
    si->mappedNodes_ = nodes;
    si->mappedNodeCount_ = nodeCount;
    si->usedBuckets_ = usedBuckets;
    si->entryCount_ = entryCount;
    return si;
}

void SpatialIndex::ensureOwned() {
    if (!mappedOwner_) return;
    if (mappedBuckets_ && mappedBucketCount_ > 0) {
        buckets_.assign(mappedBuckets_, mappedBuckets_ + mappedBucketCount_);
    } else {
        buckets_.clear();
    }
    if (mappedNodes_ && mappedNodeCount_ > 0) {
        nodes_.assign(mappedNodes_, mappedNodes_ + mappedNodeCount_);
    } else {
        nodes_.clear();
    }
    mappedOwner_.reset();
    mappedBuckets_ = nullptr;
    mappedBucketCount_ = 0;
    mappedNodes_ = nullptr;
    mappedNodeCount_ = 0;
}

std::shared_ptr<SpatialIndex> SpatialIndex::clone() const {
    auto copy = std::make_shared<SpatialIndex>();
    copy->name_ = name_;
    copy->cols_ = cols_;
    copy->dims_ = dims_;
    copy->cellSize_ = cellSize_;
    if (const Bucket* bp = bucketData(); bp && bucketCount() > 0) {
        copy->buckets_.assign(bp, bp + bucketCount());
    }
    if (const Node* np = nodeData(); np && nodeCount() > 0) {
        copy->nodes_.assign(np, np + nodeCount());
    }
    copy->freeHead_ = freeHead_;
    copy->usedBuckets_ = usedBuckets_;
    copy->entryCount_ = entryCount_;
    return copy;
}

void SpatialIndex::clear(size_t expectedItems) {
    mappedOwner_.reset();
    mappedBuckets_ = nullptr;
    mappedBucketCount_ = 0;
    mappedNodes_ = nullptr;
    mappedNodeCount_ = 0;
    size_t cap = 64;
    const size_t minCap = (expectedItems * 10) / 6 + 16;
    while (cap < minCap) cap <<= 1;
    buckets_.assign(cap, Bucket{});
    nodes_.clear();
    freeHead_ = kInvalidNode;
    usedBuckets_ = 0;
    entryCount_ = 0;
}

int32_t SpatialIndex::cellCoord(double v) const {
    const double scaled = std::floor(v / cellSize_);
    if (scaled <= -2147483000.0) return -2147483000;
    if (scaled >= 2147483000.0) return 2147483000;
    return static_cast<int32_t>(scaled);
}

uint64_t SpatialIndex::hashCell(int32_t cx, int32_t cy, int32_t cz) {
    uint64_t h = mixHash64(static_cast<uint32_t>(cx));
    h = combineHash64(h, static_cast<uint32_t>(cy));
    h = combineHash64(h, static_cast<uint32_t>(cz));
    return h;
}

void SpatialIndex::rehash(size_t newCap) {
    ensureOwned();
    std::vector<Bucket> next(newCap);
    const size_t mask = newCap - 1;
    size_t liveBuckets = 0;
    for (const Bucket& b : buckets_) {
        if (!b.occupied || b.headNode == kInvalidNode) continue;
        size_t pos = static_cast<size_t>(hashCell(b.cx, b.cy, b.cz)) & mask;
        while (next[pos].occupied) pos = (pos + 1) & mask;
        next[pos] = b;
        ++liveBuckets;
    }
    buckets_ = std::move(next);
    usedBuckets_ = liveBuckets;
}

size_t SpatialIndex::findSlot(int32_t cx, int32_t cy, int32_t cz) const {
    const Bucket* bp = bucketData();
    const size_t n = bucketCount();
    if (!bp || n == 0) return 0;
    const size_t mask = n - 1;
    size_t pos = static_cast<size_t>(hashCell(cx, cy, cz)) & mask;
    size_t steps = 0;
    while (bp[pos].occupied &&
           (bp[pos].cx != cx || bp[pos].cy != cy || bp[pos].cz != cz) &&
           steps < n) {
        pos = (pos + 1) & mask;
        ++steps;
    }
    return pos;
}

uint32_t SpatialIndex::findOccupiedHead(int32_t cx, int32_t cy, int32_t cz) const {
    const Bucket* bp = bucketData();
    const size_t n = bucketCount();
    if (!bp || n == 0) return kInvalidNode;
    const size_t pos = findSlot(cx, cy, cz);
    const Bucket& b = bp[pos];
    if (!b.occupied || b.cx != cx || b.cy != cy || b.cz != cz) return kInvalidNode;
    return b.headNode;
}

void SpatialIndex::insert(double x, double y, double z, RowLoc loc) {
    ensureOwned();
    if (!extractCoords(x, y, z, dims_)) return;
    if (buckets_.empty()) clear(64);
    if ((usedBuckets_ + 1) * 10 >= buckets_.size() * 7) {
        size_t liveBuckets = 0;
        for (const Bucket& b : buckets_) {
            if (b.occupied && b.headNode != kInvalidNode) ++liveBuckets;
        }
        const size_t nextCap = ((liveBuckets + 1) * 10 >= buckets_.size() * 6)
                                   ? (buckets_.size() * 2)
                                   : buckets_.size();
        rehash(nextCap);
    }
    const int32_t cx = cellCoord(x);
    const int32_t cy = cellCoord(y);
    const int32_t cz = (dims_ >= 3) ? cellCoord(z) : 0;
    const size_t pos = findSlot(cx, cy, cz);
    Bucket& b = buckets_[pos];
    uint32_t nodeIdx = kInvalidNode;
    if (freeHead_ != kInvalidNode) {
        nodeIdx = freeHead_;
        freeHead_ = nodes_[nodeIdx].next;
    } else {
        nodeIdx = static_cast<uint32_t>(nodes_.size());
        nodes_.emplace_back();
    }
    nodes_[nodeIdx] = Node{loc, x, y, z, kInvalidNode, 0u};
    if (!b.occupied) {
        b.occupied = 1;
        b.cx = cx;
        b.cy = cy;
        b.cz = cz;
        b.headNode = nodeIdx;
        ++usedBuckets_;
    } else {
        nodes_[nodeIdx].next = b.headNode;
        b.headNode = nodeIdx;
    }
    ++entryCount_;
}

bool SpatialIndex::erase(double x, double y, double z, RowLoc loc) {
    ensureOwned();
    if (!extractCoords(x, y, z, dims_) || buckets_.empty()) return false;
    const int32_t cx = cellCoord(x);
    const int32_t cy = cellCoord(y);
    const int32_t cz = (dims_ >= 3) ? cellCoord(z) : 0;
    const size_t pos = findSlot(cx, cy, cz);
    Bucket& b = buckets_[pos];
    if (!b.occupied) return false;
    uint32_t prev = kInvalidNode;
    uint32_t cur = b.headNode;
    while (cur != kInvalidNode) {
        if (locEq(nodes_[cur].loc, loc)) {
            if (prev == kInvalidNode) b.headNode = nodes_[cur].next;
            else nodes_[prev].next = nodes_[cur].next;
            nodes_[cur].next = freeHead_;
            freeHead_ = cur;
            --entryCount_;
            return true;
        }
        prev = cur;
        cur = nodes_[cur].next;
    }
    return false;
}

void SpatialIndex::queryAABB(double minX, double minY, double minZ,
                             double maxX, double maxY, double maxZ,
                             std::vector<RowLoc>& out) const {
    const Bucket* bp = bucketData();
    const size_t bCnt = bucketCount();
    const Node* np = nodeData();
    const size_t nCnt = nodeCount();
    if (!bp || bCnt == 0 || !np || nCnt == 0 || entryCount_ == 0 ||
        minX > maxX || minY > maxY || (dims_ >= 3 && minZ > maxZ)) {
        return;
    }
    const int32_t x0 = cellCoord(minX), x1 = cellCoord(maxX);
    const int32_t y0 = cellCoord(minY), y1 = cellCoord(maxY);
    const int32_t z0 = (dims_ >= 3) ? cellCoord(minZ) : 0;
    const int32_t z1 = (dims_ >= 3) ? cellCoord(maxZ) : 0;
    const uint64_t span = static_cast<uint64_t>(x1 - x0 + 1) *
                          static_cast<uint64_t>(y1 - y0 + 1) *
                          static_cast<uint64_t>(z1 - z0 + 1);

    auto testNode = [&](const Node& n) {
        if (n.x < minX || n.x > maxX || n.y < minY || n.y > maxY) return;
        if (dims_ >= 3 && (n.z < minZ || n.z > maxZ)) return;
        out.push_back(n.loc);
    };
    auto walkList = [&](uint32_t head) {
        uint32_t cur = head;
        size_t steps = 0;
        while (cur != kInvalidNode && cur < nCnt && steps < nCnt) {
            testNode(np[cur]);
            cur = np[cur].next;
            ++steps;
        }
    };

    if (span > 16384 || span > usedBuckets_ * 2ULL) {
        for (size_t bi = 0; bi < bCnt; ++bi) {
            const Bucket& b = bp[bi];
            if (!b.occupied) continue;
            if (b.cx < x0 || b.cx > x1 || b.cy < y0 || b.cy > y1 ||
                b.cz < z0 || b.cz > z1) {
                continue;
            }
            walkList(b.headNode);
        }
        return;
    }

    for (int32_t gz = z0; gz <= z1; ++gz) {
        for (int32_t gy = y0; gy <= y1; ++gy) {
            for (int32_t gx = x0; gx <= x1; ++gx) {
                walkList(findOccupiedHead(gx, gy, gz));
            }
        }
    }
}

void SpatialIndex::queryRadius(double cx, double cy, double cz, double radius,
                               std::vector<RowLoc>& out) const {
    const Bucket* bp = bucketData();
    const size_t bCnt = bucketCount();
    const Node* np = nodeData();
    const size_t nCnt = nodeCount();
    if (!bp || bCnt == 0 || !np || nCnt == 0 || radius < 0.0 ||
        entryCount_ == 0 || !std::isfinite(radius)) {
        return;
    }
    const double r2 = radius * radius;
    const double minX = cx - radius, maxX = cx + radius;
    const double minY = cy - radius, maxY = cy + radius;
    const double minZ = (dims_ >= 3) ? (cz - radius) : 0.0;
    const double maxZ = (dims_ >= 3) ? (cz + radius) : 0.0;

    const int32_t x0 = cellCoord(minX), x1 = cellCoord(maxX);
    const int32_t y0 = cellCoord(minY), y1 = cellCoord(maxY);
    const int32_t z0 = (dims_ >= 3) ? cellCoord(minZ) : 0;
    const int32_t z1 = (dims_ >= 3) ? cellCoord(maxZ) : 0;
    const uint64_t span = static_cast<uint64_t>(x1 - x0 + 1) *
                          static_cast<uint64_t>(y1 - y0 + 1) *
                          static_cast<uint64_t>(z1 - z0 + 1);

    auto checkNode = [&](const Node& n) {
        const double dx = n.x - cx;
        const double dy = n.y - cy;
        const double dz = (dims_ >= 3) ? (n.z - cz) : 0.0;
        if (dx * dx + dy * dy + dz * dz <= r2) {
            out.push_back(n.loc);
        }
    };
    auto walkList = [&](uint32_t head) {
        uint32_t cur = head;
        size_t steps = 0;
        while (cur != kInvalidNode && cur < nCnt && steps < nCnt) {
            checkNode(np[cur]);
            cur = np[cur].next;
            ++steps;
        }
    };

    if (span > 16384 || span > usedBuckets_ * 2ULL) {
        for (size_t bi = 0; bi < bCnt; ++bi) {
            const Bucket& b = bp[bi];
            if (!b.occupied) continue;
            if (b.cx < x0 || b.cx > x1 || b.cy < y0 || b.cy > y1 ||
                b.cz < z0 || b.cz > z1) {
                continue;
            }
            walkList(b.headNode);
        }
        return;
    }

    for (int32_t gz = z0; gz <= z1; ++gz) {
        for (int32_t gy = y0; gy <= y1; ++gy) {
            for (int32_t gx = x0; gx <= x1; ++gx) {
                walkList(findOccupiedHead(gx, gy, gz));
            }
        }
    }
}

void SpatialIndex::queryNearest(double cx, double cy, double cz, size_t k, double maxRadius,
                                std::vector<std::pair<RowLoc, double>>& out) const {
    out.clear();
    const Bucket* bp = bucketData();
    const size_t bCnt = bucketCount();
    const Node* np = nodeData();
    const size_t nCnt = nodeCount();
    if (!bp || bCnt == 0 || !np || nCnt == 0 || k == 0 || entryCount_ == 0 ||
        !std::isfinite(cx) || !std::isfinite(cy)) {
        return;
    }
    if (dims_ >= 3 && !std::isfinite(cz)) return;

    struct Cand {
        double dist2;
        RowLoc loc;
        bool operator<(const Cand& o) const { return dist2 < o.dist2; }
    };
    std::priority_queue<Cand> heap;
    const bool hasMaxR = std::isfinite(maxRadius) && maxRadius >= 0.0;
    const double maxR2 = hasMaxR ? maxRadius * maxRadius : std::numeric_limits<double>::infinity();

    auto consider = [&](const Node& n) {
        const double dx = n.x - cx;
        const double dy = n.y - cy;
        const double dz = (dims_ >= 3) ? (n.z - cz) : 0.0;
        const double d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > maxR2) return;
        if (heap.size() < k) {
            heap.push(Cand{d2, n.loc});
        } else if (d2 < heap.top().dist2) {
            heap.pop();
            heap.push(Cand{d2, n.loc});
        }
    };
    auto walkList = [&](uint32_t head) {
        uint32_t cur = head;
        size_t steps = 0;
        while (cur != kInvalidNode && cur < nCnt && steps < nCnt) {
            consider(np[cur]);
            cur = np[cur].next;
            ++steps;
        }
    };

    const int32_t gcx = cellCoord(cx);
    const int32_t gcy = cellCoord(cy);
    const int32_t gcz = (dims_ >= 3) ? cellCoord(cz) : 0;
    const int32_t maxRing = hasMaxR
                                ? static_cast<int32_t>(std::ceil(maxRadius / cellSize_)) + 1
                                : 16;

    int32_t ring = 0;
    bool provedOptimal = false;
    while (ring <= maxRing && ring <= 32) {
        const int32_t zLo = (dims_ >= 3) ? (gcz - ring) : 0;
        const int32_t zHi = (dims_ >= 3) ? (gcz + ring) : 0;
        for (int32_t gz = zLo; gz <= zHi; ++gz) {
            for (int32_t gy = gcy - ring; gy <= gcy + ring; ++gy) {
                for (int32_t gx = gcx - ring; gx <= gcx + ring; ++gx) {
                    const bool onShell =
                        std::abs(gx - gcx) == ring || std::abs(gy - gcy) == ring ||
                        (dims_ >= 3 && std::abs(gz - gcz) == ring);
                    if (!onShell) continue;
                    walkList(findOccupiedHead(gx, gy, gz));
                }
            }
        }
        if (heap.size() == k) {
            const double minNextDist = std::max(0.0, static_cast<double>(ring) * cellSize_ - cellSize_);
            if (heap.top().dist2 <= minNextDist * minNextDist) {
                provedOptimal = true;
                break;
            }
        }
        ++ring;
    }
    const bool scannedAll = !provedOptimal && (!hasMaxR || ring <= maxRing);

    if (scannedAll) {
        while (!heap.empty()) heap.pop();
        for (size_t bi = 0; bi < bCnt; ++bi) {
            const Bucket& b = bp[bi];
            if (!b.occupied) continue;
            walkList(b.headNode);
        }
    }

    out.reserve(heap.size());
    while (!heap.empty()) {
        const Cand c = heap.top();
        heap.pop();
        out.emplace_back(c.loc, std::sqrt(c.dist2));
    }
    std::reverse(out.begin(), out.end());
}

}  // namespace bro::table
