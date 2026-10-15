// The agent view's text model (engine/agent_view.h, docs/agent-view.md):
// lines and their parse, revisions, and the diffs between views.

#include "engine/agent_view.h"

#include <cstdio>
#include <string>

using namespace bro::engine::view;

namespace {

int g_checks = 0, g_failures = 0;

void check(bool ok, const char* what, const std::string& detail = {}) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s%s%s\n", what, detail.empty() ? "" : "\n  got: ", detail.c_str());
    }
}

Item el(const char* handle, int depth, const char* role, const char* name, const char* flags = "") {
    Item it;
    it.handle = handle;
    it.depth = depth;
    it.role = role;
    it.name = name;
    it.flags = flags;
    it.hasRect = true;
    it.x = 10;
    it.y = 20;
    it.w = 100;
    it.h = 30;
    return it;
}

Item win(const char* handle, const char* app, const char* title, bool focused) {
    Item it;
    it.handle = handle;
    it.role = app;
    it.name = title;
    if (focused) it.flags = "*";
    it.hasRect = true;
    it.w = 800;
    it.h = 600;
    return it;
}

void testHandles() {
    check(isElementHandle("3.4") && !isElementHandle("w3") && !isElementHandle("3.") && !isElementHandle("a.b"),
          "element handles are digits.digits");
    check(isWindowHandle("w0") && isWindowHandle("w12") && !isWindowHandle("w") && !isWindowHandle("3.4"),
          "window handles are w and digits");
    check(windowOf("w3") == 3 && windowOf("12.7") == 12 && windowOf("x") == -1, "windowOf");
    check(elementOf("12.7") == 7 && elementOf("w3") == 0, "elementOf");
}

void testLines() {
    Item f = el("3.1", 0, "textbox", "Search", "@");
    f.value = "snow man";
    f.hasValue = true;
    const std::string line = formatItem(f, false);
    check(line == " 3.1 textbox@ \"Search\" =\"snow man\"", "a field's line", line);
    check(formatItem(f, true) == " 3.1 textbox@ \"Search\" =\"snow man\" 10,20 100x30", "with its rect",
          formatItem(f, true));

    Item w = win("w3", "helm.charmap", "Character Map", true);
    check(formatItem(w, false) == "w3* helm.charmap \"Character Map\" 0,0 800x600",
          "a window line: flags on the handle, and its rect", formatItem(w, false));
    Item wb;
    check(parseItem(formatItem(w, false), wb) && wb.handle == "w3" && wb.flags == "*" && wb.role == "helm.charmap",
          "a window line parses back");

    Item q = el("2.5", 2, "text", "say \"hi\"\\now\nthere", "");
    q.extra = "3 of 9";
    Item back;
    check(parseItem(formatItem(q, true), back), "a line parses back");
    check(back.handle == "2.5" && back.depth == 2 && back.role == "text" && back.name == "say \"hi\"\\now there" &&
              back.extra == "3 of 9" && back.hasRect && back.w == 100,
          "the parse is the item (newline as space)", back.name);

    Item fl = el("1.1", 0, "checkbox", "Remember", "*!");
    check(parseItem(formatItem(fl, false), back) && back.role == "checkbox" && back.flags == "*!" && !back.hasRect,
          "flags parse apart from the role");

    std::string longName(200, 'x');
    Item ln = el("1.2", 0, "button", longName.c_str());
    const std::string cut = formatItem(ln, false);
    check(cut.find("\xE2\x80\xA6") != std::string::npos && cut.size() < 110, "a long name is cut", cut);
    check(truncateUtf8("\xE2\x98\x83\xE2\x98\x83\xE2\x98\x83", 2) == "\xE2\x98\x83\xE2\x98\x83\xE2\x80\xA6",
          "truncation counts code points");

    Snapshot s{win("w0", "app", "T", true), el("0.1", 0, "button", "OK"), el("0.2", 1, "text", "hi")};
    const std::string body = formatBody(s, false);
    check(body == "w0* app \"T\" 0,0 800x600\nw0:\n 0.1 button \"OK\"\n  0.2 text \"hi\"\n",
          "a body has a section line before a window's elements", body);
    const Snapshot parsed = parseItems("@view r3\n" + body);
    check(parsed.size() == 3 && parsed[2].depth == 1, "parseItems skips headers and section lines");
}

void testDiffs() {
    History h;
    Snapshot a{win("w0", "app", "T", true), el("0.1", 0, "textbox", "Search"), el("0.2", 0, "button", "Go"),
               el("0.3", 0, "text", "old")};
    a[1].hasValue = true;
    const uint64_t r1 = h.commit("default", a);
    check(r1 == 1, "the first revision is r1");
    check(h.reply("default", 0, false).rfind("@view r1\n", 0) == 0, "a full view starts with its header");
    check(h.reply("default", r1, false) == "@view r1 =\n", "nothing changed: one line", h.reply("default", r1, false));

    // The same view again keeps the revision; a rect-only change does too.
    Snapshot moved = a;
    moved[2].x = 400;
    check(h.commit("default", moved) == r1, "a moved element is not a new revision");

    Snapshot b = a;
    b[1].value = "snow";
    b.erase(b.begin() + 3);                       // 0.3 removed
    b.push_back(el("0.4", 0, "status", "Copied"));  // 0.4 added
    b[2].flags = "!";                             // 0.2 now disabled
    const uint64_t r2 = h.commit("default", b);
    check(r2 == 2, "a change is a new revision");
    const std::string d = h.reply("default", r1, false);
    check(d.rfind("@view r2 +1 -1 ~2\n", 0) == 0, "the diff header counts", d);
    check(d.find("+ 0.4 status \"Copied\"\n") != std::string::npos, "an added line", d);
    check(d.find("- 0.3\n") != std::string::npos, "a removed handle", d);
    check(d.find("~ 0.1 =\"snow\"\n") != std::string::npos, "a changed value names only the value", d);
    check(d.find("~ 0.2 button!\n") != std::string::npos, "a changed flag names the role and flags", d);
    check(h.reply("default", r2, false) == "@view r2 =\n", "no change since r2");

    // Several removals share one line.
    Snapshot c2{a[0], b[1]};
    h.commit("default", c2);
    const std::string d2 = h.reply("default", r2, false);
    check(d2.find("- 0.2 0.4\n") != std::string::npos, "removals on one line", d2);

    // A window with none of its elements left is one "N.*".
    Snapshot c{a[0]};
    const uint64_t r3 = h.commit("default", c);
    const std::string d3 = h.reply("default", r2, false);
    check(d3.find("- 0.*\n") != std::string::npos && d3.rfind("@view r", 0) == 0 && d3.find(" -3 ") != std::string::npos,
          "a window's elements all gone collapse to N.*", d3);
    (void)r3;

    // Flags turning off say so; cleared notes say so.
    {
        Snapshot f0{el("0.1", 0, "textbox", "Name", "@"), el("0.2", 0, "button", "Go", "!"),
                    el("0.3", 0, "listbox", "L")};
        f0[2].extra = "20 of 44";
        Snapshot f1 = f0;
        f1[0].flags = "";
        f1[1].flags = "*";
        f1[2].extra = "";
        const std::string fd = formatDiff(f0, f1, false, nullptr);
        check(fd.find("~ 0.1 textbox -@\n") != std::string::npos, "a lost flag is -x", fd);
        check(fd.find("~ 0.2 button* -!\n") != std::string::npos, "gained and lost flags", fd);
        check(fd.find("~ 0.3 [none]\n") != std::string::npos, "notes cleared are [none]", fd);
        Snapshot w0{win("w0", "shell", "S", true)}, w1{win("w0", "shell", "S", false)};
        check(formatDiff(w0, w1, false, nullptr) == "~ w0 -*\n", "a window losing focus",
              formatDiff(w0, w1, false, nullptr));
        check(formatDiff(w1, w0, false, nullptr) == "~ w0*\n", "a window gaining focus",
              formatDiff(w1, w0, false, nullptr));
    }

    // Runs of sibling lines of one role join into one line, and parse back.
    {
        Snapshot k{win("w0", "app", "T", true), el("0.4", 1, "button", "7"), el("0.5", 1, "button", "8"),
                   el("0.6", 1, "button", "9"), el("0.7", 1, "button", "x", "!"), el("0.8", 1, "text", "a")};
        const std::string body = formatBody(k, false);
        check(body.find("\n  0.4-6 button \"7\" \"8\" \"9\"\n  0.7 button! \"x\"\n") != std::string::npos,
              "a run of three is one line", body);
        const Snapshot back = parseItems(body);
        check(back.size() == k.size() && back[2].handle == "0.5" && back[2].name == "8" && back[2].depth == 1 &&
                  back[3].role == "button",
              "a run line parses back into its items");
        check(formatBody(k, true).find("0.4-6") == std::string::npos, "no runs with rects");
        Snapshot k0{k[0]};
        const std::string kd = formatDiff(k0, k, false, nullptr);
        check(kd.find("+ 0.4-6 button \"7\" \"8\" \"9\"\n") != std::string::npos, "added runs join too", kd);
        const std::string kr = formatDiff(k, k0, false, nullptr);
        check(kr == "- 0.*\n", "all gone", kr);
        Snapshot k2{k[0], k[5]};
        const std::string kr2 = formatDiff(k, k2, false, nullptr);
        check(kr2 == "- 0.4-7\n", "removed handles in a row are a range", kr2);
        Snapshot k3 = k;
        k3[1].name = "a";
        k3[2].name = "b";
        k3[3].name = "c";
        k3[5].name = "z";
        const std::string kc = formatDiff(k, k3, false, nullptr);
        check(kc == "~ 0.4-6 \"a\" \"b\" \"c\"\n~ 0.8 \"z\"\n", "renamed lines in a row are one ~ line", kc);
    }

    // An added subtree keeps its nesting, relative to its top.
    {
        Snapshot n0{el("0.1", 2, "button", "Open")};
        Snapshot n1{el("0.1", 2, "button", "Open"), el("0.5", 2, "dialog", "Add"), el("0.6", 3, "textbox", "Hour"),
                    el("0.7", 4, "text", "deep"), el("0.8", 3, "button", "OK")};
        const std::string nd = formatDiff(n0, n1, false, nullptr);
        check(nd == "+ 0.5 dialog \"Add\"\n+  0.6 textbox \"Hour\"\n+   0.7 text \"deep\"\n+  0.8 button \"OK\"\n",
              "added lines keep their relative indent", nd);
    }

    // Expired: older than the history keeps.
    History small(2);
    small.commit("s", a);
    small.commit("s", b);
    const uint64_t last = small.commit("s", c);
    const std::string e = small.reply("s", 1, false);
    check(e.rfind("@view r" + std::to_string(last) + " full\n", 0) == 0, "an expired revision gets the full view", e);

    // Scopes keep their own snapshots; a revision of another scope is unknown here.
    History two;
    two.commit("default", a);
    two.commit("w3", b);
    check(two.reply("w3", 1, false).find(" full\n") != std::string::npos, "revisions belong to their scope");

    // A window's move is a change (its rect is part of its line).
    History wh;
    Snapshot w1{win("w2", "clock", "Clock", false)};
    const uint64_t a1 = wh.commit("d", w1);
    w1[0].x = 300;
    wh.commit("d", w1);
    const std::string wd = wh.reply("d", a1, false);
    check(wd.find("~ w2 300,0 800x600\n") != std::string::npos, "a moved window", wd);

    // With rects, an element's rect counts in the diff.
    Snapshot r0{el("0.1", 0, "button", "A")};
    Snapshot rr = r0;
    rr[0].y = 99;
    DiffCounts cnt;
    check(formatDiff(r0, rr, false, &cnt).empty() && cnt.empty(), "without rects a move is no change");
    check(formatDiff(r0, rr, true, &cnt) == "~ 0.1 10,99 100x30\n", "with rects it is",
          formatDiff(r0, rr, true, &cnt));
}

}  // namespace

int main() {
    testHandles();
    testLines();
    testDiffs();
    std::printf("%s: %d checks, %d failed\n", g_failures ? "FAIL" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
