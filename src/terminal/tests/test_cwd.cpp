// OSC 7's URI as a path (term_cwd.h): percent-decoding, hosts, Windows
// drive letters, other schemes and bare paths.

#include "check.h"
#include "tests.h"

#include "terminal/term_cwd.h"

namespace bro::terminal::test {

namespace {

bool is(std::string_view uri, const std::string& path, const std::string& host) {
    const CwdLocation c = cwdFromUri(uri);
    const bool ok = c.path == path && c.host == host;
    if (!ok) std::printf("    %.*s -> path \"%s\" host \"%s\"\n", int(uri.size()), uri.data(), c.path.c_str(), c.host.c_str());
    return ok;
}

} // namespace

void run_cwd_tests() {
    section("cwd: OSC 7 URI to path");
    CHECK(is("file:///home/me", "/home/me", ""));
    CHECK(is("file://box/home/me/src", "/home/me/src", "box"));
    CHECK(is("file://localhost/tmp", "/tmp", "localhost"));
    CHECK(is("FILE://box/tmp", "/tmp", "box"));
    // Percent-decoding, UTF-8 included; a malformed escape stays.
    CHECK(is("file://box/home/me/My%20Files/%E2%9C%93", "/home/me/My Files/\xE2\x9C\x93", "box"));
    CHECK(is("file://box/a%2/b%zz%", "/a%2/b%zz%", "box"));
    // Query and fragment end the path; encoded they are part of it.
    CHECK(is("file://box/a/b?x=1#y", "/a/b", "box"));
    CHECK(is("file://box/a%23b", "/a#b", "box"));
    // Windows drives, in Windows form.
    CHECK(is("file:///C:/Users/me", "C:\\Users\\me", ""));
    CHECK(is("file://pc/c:/Program%20Files/x", "C:\\Program Files\\x", "pc"));
    CHECK(is("file:///D|/x", "D:\\x", ""));
    CHECK(is("file:///c:", "C:\\", ""));
    CHECK(is("file:///c:/", "C:\\", ""));
    CHECK(is("file:///cd/x", "/cd/x", ""));
    // Other schemes: host and path, not decoded.
    CHECK(is("kitty-shell-cwd://box/home/me/a b", "/home/me/a b", "box"));
    CHECK(is("kitty-shell-cwd://box/a%20b", "/a%20b", "box"));
    // No path, no URI.
    CHECK(is("file://box", "", "box"));
    CHECK(is("", "", ""));
    CHECK(is("/plain/path", "/plain/path", ""));
    CHECK(is("C:\\plain", "C:\\plain", ""));
}

} // namespace bro::terminal::test
