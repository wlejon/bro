#pragma once

// The agent view's text model (docs/agent-view.md): the lines of a view,
// their format and parse, revisions, and the diff between two views. Pure:
// no DOM, no engine, so it is tested on its own (tests/test_agent_view.cpp).
//
//   @view r42                                   a full view
//   w3* helm.charmap "Character Map" 300,120 900x600
//   w3:
//    3.1 textbox@ "Search" ="snowman"
//    3.2 list "Characters" [1 of 1]
//     3.3 option* "SNOWMAN U+2603"
//
//   @view r45 +1 -1 ~1                          a diff since a revision
//   + 3.6 status "Copied"
//   - 3.3
//   ~ 3.1 ="snow"
//
//   @view r45 =                                 nothing changed since r45
//   @view r45 full                              too old to diff: the full view

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace bro::engine::view {

/// One line of a view: a window ("w3") or an element ("3.4").
struct Item {
    std::string handle;
    int depth = 0;          // nesting under the window (elements), 0 for windows
    std::string role;       // an element's role; a window's app id
    std::string flags;      // * selected/checked/focused window, ! disabled, ^ expanded, @ keyboard focus
    std::string name;
    std::string value;
    bool hasValue = false;
    std::string extra;      // "[20 of 232]", "min", "no-view"
    bool hasRect = false;
    float x = 0, y = 0, w = 0, h = 0;

    bool isWindow() const { return !handle.empty() && handle[0] == 'w'; }
};

using Snapshot = std::vector<Item>;

/// Names longer than this are cut, with "…".
inline constexpr size_t kMaxNameChars = 80;

/// `s` cut to `maxChars` code points (UTF-8 safe), "…" marking the cut.
std::string truncateUtf8(std::string_view s, size_t maxChars);
/// `"s"` with \ and " escaped and control characters as spaces.
std::string quote(std::string_view s);

/// The window number of a handle: "w3" -> 3, "3.4" -> 3; -1 when neither.
int windowOf(std::string_view handle);
/// The element number of an element handle: "3.4" -> 4; 0 for others.
uint32_t elementOf(std::string_view handle);
/// Whether `s` is an element handle (digits '.' digits) or a window (w digits).
bool isElementHandle(std::string_view s);
bool isWindowHandle(std::string_view s);

/// One line, indented (elements one space more than their depth). Window
/// lines always carry their rect; elements only with `rects`.
std::string formatItem(const Item& item, bool rects);
/// The inverse of formatItem (rects included when present). False for a
/// line that is no item ("w3:" section lines, headers, blank).
bool parseItem(std::string_view line, Item& out);
/// Every item of a view's text (the header and section lines skipped).
Snapshot parseItems(std::string_view text);

/// The body of a full view: the items in order, a "wN:" line before each
/// window's elements.
std::string formatBody(const Snapshot& snap, bool rects);

/// Equal as the view shows them (rects only when `rects`).
bool sameItem(const Item& a, const Item& b, bool rects);

struct DiffCounts {
    int added = 0, removed = 0, changed = 0;
    bool empty() const { return !added && !removed && !changed; }
};
/// The diff lines from `from` to `to`: "+ <line>" for each added item, one
/// "- h h h" line naming the removed, "~ <handle> <changed fields>".
std::string formatDiff(const Snapshot& from, const Snapshot& to, bool rects, DiffCounts* counts);

/// Revisions of views. Each scope (the default desktop view, one window, a
/// paged list) keeps its last few snapshots; revision numbers are shared, so
/// a revision names one snapshot of one scope.
class History {
public:
    explicit History(size_t keepPerScope = 16) : keep_(keepPerScope) {}

    /// Records `snap` as the scope's current view and returns its revision:
    /// the latest one, unchanged, when nothing visible differs from it (its
    /// rects are refreshed, since rects are not revisioned).
    uint64_t commit(const std::string& scope, Snapshot snap);

    /// The reply for the scope's current view, as seen by a client holding
    /// `since` (0: none, so the full view). commit() first.
    /// An action's answer is the same call with the revision from before it.
    std::string reply(const std::string& scope, uint64_t since, bool rects) const;

    const Snapshot* find(const std::string& scope, uint64_t rev) const;
    const Snapshot* latest(const std::string& scope, uint64_t* rev = nullptr) const;
    uint64_t lastRevision() const { return next_ - 1; }

private:
    struct Entry {
        uint64_t rev;
        Snapshot snap;
    };
    size_t keep_;
    uint64_t next_ = 1;
    std::map<std::string, std::deque<Entry>> scopes_;
};

/// "@view r45 +1 -2 ~3" style headers.
std::string header(uint64_t rev);
std::string noChange(uint64_t rev);

}  // namespace bro::engine::view
