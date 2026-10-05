// The session's search against bropty's own: the same bytes fed to a
// TermSession (searched on its parser thread, stepped through history) and
// to a bare bropty::Terminal searched by bropty::Search with the same
// matcher must find exactly the same matches, in every mode the element
// offers (literal / regex, case modes, whole word), and next / previous
// must walk all of them.

#include "check.h"
#include "tests.h"

#include "terminal/term_session.h"

#include <bropty/search.h>
#include <bropty/search_regex.h>
#include <bropty/terminal.h>

#include <chrono>
#include <functional>
#include <set>
#include <string>
#include <thread>

namespace bro::terminal::test {

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

bool waitUntil(const std::function<bool()>& pred, std::chrono::milliseconds limit = 10s) {
    const auto until = Clock::now() + limit;
    while (Clock::now() < until) {
        if (pred()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return pred();
}

std::string corpus() {
    std::string s;
    // Enough lines that most of them are in history (rows 8).
    for (int i = 0; i < 400; ++i) {
        s += "line " + std::to_string(i) + ": ";
        if (i % 7 == 0) s += "Error: disk full ";
        if (i % 11 == 0) s += "error-code=" + std::to_string(i * 3) + " ";
        if (i % 13 == 0) s += "terrorize the errors ";
        if (i % 17 == 0) s += "\x1b[31mERROR\x1b[0m in red ";
        if (i % 19 == 0) s += "caf\xc3\xa9 CAF\xc3\x89 ";
        if (i % 23 == 0) s += "a.e.r.b ";
        s += "\r\n";
    }
    s += "tail line with error at the end";
    return s;
}

using Key = std::pair<std::pair<int64_t, int>, std::pair<int64_t, int>>;
Key key(const bropty::RowRange& r) { return {{r.start.row, r.start.col}, {r.end.row, r.end.col}}; }

struct Mode {
    const char* name;
    std::string pattern;
    SearchOptions session;
    bropty::RegexSearchOptions bropty;
};

std::vector<Mode> modes() {
    std::vector<Mode> m;
    auto add = [&](const char* name, std::string pattern, bool regex, SearchOptions::Case c, bool whole) {
        Mode mode{name, std::move(pattern), {}, {}};
        mode.session.regex = regex;
        mode.session.caseMode = c;
        mode.session.wholeWord = whole;
        mode.bropty.literal = !regex;
        mode.bropty.whole_word = whole;
        mode.bropty.case_mode = c == SearchOptions::Case::Sensitive     ? bropty::SearchCase::Sensitive
                              : c == SearchOptions::Case::Insensitive ? bropty::SearchCase::Insensitive
                                                                      : bropty::SearchCase::Smart;
        m.push_back(std::move(mode));
    };
    using C = SearchOptions::Case;
    add("literal smart", "error", false, C::Smart, false);
    add("literal smart, upper", "Error", false, C::Smart, false);
    add("literal sensitive", "ERROR", false, C::Sensitive, false);
    add("literal insensitive", "ERROR", false, C::Insensitive, false);
    add("literal whole word", "error", false, C::Insensitive, true);
    add("literal dots", "e.r", false, C::Smart, false);  // literal: no regex
    add("regex", "error-code=\\d+", true, C::Smart, false);
    add("regex dot", "e.r", true, C::Smart, false);
    add("regex whole word", "err\\w*", true, C::Insensitive, true);
    add("non-ASCII", "caf\xc3\xa9", false, C::Insensitive, false);
    return m;
}

void testAgainstBropty() {
    section("session: search matches bropty::Search");
    const std::string bytes = corpus();
    TermSession s(60, 8, 2000);
    s.feed(bytes);
    bropty::Terminal term(60, 8, 2000);
    term.feed(bytes);
    CHECK(waitUntil([&] { return s.screenText().find("tail line") != std::string::npos; }));

    for (const Mode& mode : modes()) {
        const std::string name = std::string(mode.name) + " /" + mode.pattern + "/";
        // bropty's own, run to completion here.
        std::string err;
        auto matcher = bropty::RegexMatcher::create(mode.pattern, mode.bropty, &err);
        CHECK_MSG(matcher != nullptr, name + ": " + err);
        if (!matcher) continue;
        bropty::Search ref(term);
        ref.start(matcher);
        while (ref.step()) {
        }
        std::set<Key> want;
        for (size_t i = 0; i < ref.size(); ++i) want.insert(key(ref.at(i)));

        // The session's, stepped on its own thread.
        CHECK_MSG(s.searchStart(mode.pattern, mode.session, &err), name + ": " + err);
        CHECK_MSG(waitUntil([&] { return s.searchStatus().complete; }), name + ": the session's search completes");
        const SearchStatus st = s.searchStatus();
        CHECK_MSG(st.active && st.pattern == mode.pattern, name + ": status");
        CHECK_MSG(st.count == want.size() && !want.empty(),
                  name + ": " + std::to_string(st.count) + " matches, bropty " + std::to_string(want.size()));

        // Walking forward visits every match once and wraps; backward too.
        for (bool backward : {false, true}) {
            std::set<Key> seen;
            std::optional<bropty::RowRange> first;
            for (size_t i = 0; i < want.size() + 1; ++i) {
                std::optional<bropty::RowRange> r = s.searchNext(backward);
                if (!r) break;
                if (i == want.size()) {
                    CHECK_MSG(first && key(*r) == key(*first), name + ": wraps to the first match");
                    break;
                }
                if (!first) first = r;
                seen.insert(key(*r));
                const SearchStatus now = s.searchStatus();
                CHECK_MSG(now.currentRange && key(*now.currentRange) == key(*r), name + ": current follows next");
            }
            CHECK_MSG(seen == want, name + (backward ? ": previous" : ": next") + " walks all " +
                                        std::to_string(want.size()) + ", saw " + std::to_string(seen.size()));
        }
    }

    // A bad regex is an error, not a search.
    std::string err;
    SearchOptions bad;
    bad.regex = true;
    CHECK(!s.searchStart("(unclosed", bad, &err) && !err.empty());
    s.searchClear();
    CHECK(!s.searchStatus().active);
}

void testFollowsOutput() {
    section("session: search follows new output");
    TermSession s(40, 6, 500);
    s.feed("needle one\r\nhay\r\n");
    std::string err;
    CHECK(s.searchStart("needle", SearchOptions{}, &err));
    CHECK(waitUntil([&] { return s.searchStatus().complete && s.searchStatus().count == 1; }));
    s.feed("more hay, a needle\r\nand NEEDLE\r\n");
    CHECK_MSG(waitUntil([&] { return s.searchStatus().count == 3; }),
              "count after output: " + std::to_string(s.searchStatus().count));
    for (int i = 0; i < 20; ++i) s.feed("scrolling hay\r\n");
    CHECK_MSG(waitUntil([&] { return s.searchStatus().count == 3; }), "matches kept as they scroll into history");
}

} // namespace

void run_session_search_tests() {
    testAgainstBropty();
    testFollowsOutput();
}

} // namespace bro::terminal::test
