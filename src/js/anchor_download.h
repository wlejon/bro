#pragma once

#include <quickjs.h>
#include <string>

namespace bro::dom { class Element; }

namespace bro::js {

/// The `<a download>` activation behavior: save the link's bytes to a file
/// instead of navigating to them.
///
/// This is how the web hands a file *out*. A page builds the bytes it wants to
/// export, wraps them in a Blob, points an anchor at an object URL for it, and
/// clicks the anchor — the whole "Export / Save as / Download" pattern, and the
/// only one a page has. Without it the export runs, serializes correctly,
/// resolves its promise, and produces nothing, which is indistinguishable from
/// success right up until the user goes looking for the file.
///
/// `el` is the clicked element; the nearest ancestor anchor carrying a
/// `download` attribute (the anchor is often wrapped around an icon or a span)
/// is the one that acts. Returns true if a download was started.
///
/// The bytes come from wherever the href points: an object URL or a `data:`
/// URL resolves in memory, anything else is read as an app path. The file
/// lands in the user's Downloads folder under the `download` attribute's
/// value, or the URL's own filename when the attribute is empty; an existing
/// file of that name is never overwritten — " (2)", " (3)" and so on are tried.
bool runAnchorDownload(JSContext* ctx, dom::Element* el);

/// Where the last download landed (absolute path), or empty if none has
/// happened. Headless exposes it as `lastDownload()` so a test can assert on
/// an export without knowing the user's folder layout.
const std::string& lastDownloadPath();

} // namespace bro::js
