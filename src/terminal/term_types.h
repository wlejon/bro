#pragma once
// Plain types the terminal session hands the element: what the program told
// the embedder (events), the scrollback view's position, search options and
// status, and the shell-integration command list. No DOM, no threads.

#include <bropty/position.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bro::terminal {

// What a program may do with the clipboard through OSC 52.
enum class ClipboardPolicy : uint8_t {
    Deny,       // neither: writes are dropped, reads refused
    WriteOnly,  // writes reach the page (clipboardwrite), reads are refused
    ReadWrite,  // reads too: the page answers each request
};

// Something the program said to the embedder, queued on the parser thread
// and delivered on the main thread (TermSession::takeEvents).
struct TermEvent {
    enum class Kind : uint8_t {
        Title,           // text: the new title (OSC 0 / 2)
        Cwd,             // text: the URI (OSC 7)
        Bell,            // BEL
        Notification,    // text: body, title, id, source ("osc9", "osc777", "osc99"), number: urgency
        Progress,        // number: state (0 clear .. 4 paused), value: 0..100 (OSC 9;4)
        ClipboardWrite,  // text: the data, selection: the OSC 52 Pc field
        ClipboardRead,   // id: the request to answer, selection: the Pc field
        PromptMark,      // kind: 'A' 'B' 'C' 'D', params: the mark's parameters
        PointerShape,    // text: the CSS cursor name the program asked for ("" = default)
    };
    Kind kind = Kind::Bell;
    std::string text;
    std::string title;
    std::string id;
    std::string source;
    std::string selection;
    std::string params;
    char mark = 0;
    int number = 0;
    int value = 0;
    uint64_t request = 0;
};

// Where the view is in the buffer, as absolute rows (bropty/position.h).
struct ViewState {
    int64_t topRow = 0;        // the row shown at the top
    int64_t firstRow = 0;      // the oldest row the view can reach
    int64_t screenTopRow = 0;  // the live screen's first row (topRow there = at the bottom)
    int rows = 0;              // rows on screen
    bool atBottom = true;
    bool altScreen = false;
    // History rows above the screen the view can scroll through.
    [[nodiscard]] int64_t historyRows() const { return screenTopRow - firstRow; }
    bool operator==(const ViewState&) const = default;
};

struct SearchOptions {
    bool regex = false;
    enum class Case : uint8_t { Smart, Sensitive, Insensitive } caseMode = Case::Smart;
    bool wholeWord = false;
};

struct SearchStatus {
    bool active = false;
    bool complete = false;    // the whole buffer has been matched
    size_t count = 0;
    std::optional<size_t> current;  // index of the current match, in buffer order
    std::optional<bropty::RowRange> currentRange;
    std::string pattern;
    bool operator==(const SearchStatus&) const = default;
};

// One command of the shell's integration marks (OSC 133).
struct CommandInfo {
    bropty::RowPos prompt;
    std::optional<bropty::RowPos> input, output, end;
    std::optional<int> exitCode;
    bool finished = false;
    std::string commandLine;
};

// A link under a cell: an OSC 8 hyperlink, a detected URL or a path.
struct LinkInfo {
    bropty::RowRange range;
    std::string kind;    // "hyperlink" | "url" | "path"
    std::string target;  // the URI, or the path as written
    std::string text;    // the link's text on screen
};

} // namespace bro::terminal
