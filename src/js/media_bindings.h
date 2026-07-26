#pragma once

#include <quickjs.h>

#include <string>

namespace bro::js {

/// Install `bro.media` — whole-file analysis of a media file: the waveform and
/// the filmstrip a timeline is drawn from. Both go through the media backend
/// registry, so a host that registered its own backend gets them for every
/// format it can open.
///
/// Installed in worker realms too: the analysis is a synchronous full-file
/// decode, which is exactly the kind of work that belongs off the UI thread.
/// `basePath` resolves relative paths, matching Image and fetch.
void installMediaBindings(JSContext* ctx, const std::string& basePath);

} // namespace bro::js
