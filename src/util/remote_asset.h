#pragma once

#include <string>

namespace bro::util {

/// Absolute-URL handling for the page's own resources: `<script src>`,
/// `<link rel=stylesheet href>`, and ES module specifiers.
///
/// bro apps ship their assets, so nearly every path in a manifest is a file.
/// But real pages — the three.js editor among them — pull a few things from a
/// CDN, and a URL joined onto the app directory is not a path anything can
/// open. These helpers keep URLs whole through resolution and then fetch them
/// through an on-disk cache, so the first run pays for the network and every
/// run after it is offline and deterministic.
///
/// This is the *document's* loader, not `fetch()`. Nothing here is a new
/// capability — an app already has the network and already runs code from its
/// own directory — it just makes the page's own <script> tags mean what the
/// web says they mean.

/// Does this string start with a URL scheme (`scheme://`)? Deliberately
/// narrower than "contains a colon": `C:\app` is a drive letter, not a scheme.
bool hasUrlScheme(const std::string& s);

/// Is this an http(s) URL — the only kind the document loader will fetch?
bool isHttpUrl(const std::string& s);

/// Resolve `rel` against the URL `base`, per RFC 3986: a full URL passes
/// through, `//host/x` inherits the scheme, `/x` the origin, anything else is
/// relative to base's directory. Dot segments are removed. Returns `rel`
/// unchanged when `base` is not a URL.
std::string resolveUrl(const std::string& base, const std::string& rel);

/// Fetch `url`, going through the cache in the OS user-data dir
/// (`<userdata>/bro/remote-cache/`). Returns the body, or an empty string on
/// failure. A cached copy is used when the network fails, so an app that
/// booted once keeps booting on a plane.
///
/// Blocking, by design: the document loader is synchronous, and a page's
/// `<script src>` tags are ordered.
std::string fetchRemoteCached(const std::string& url);

/// Where a fetched URL lands on disk. Exposed for tests and for tooling that
/// wants to pre-seed or clear the cache.
std::string remoteCachePath(const std::string& url);

/// True when the build has no HTTP client compiled in, so every remote asset
/// resolves from cache or not at all.
bool remoteFetchAvailable();

} // namespace bro::util
