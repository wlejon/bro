#pragma once

#include <map>
#include <string>

namespace bro::util {

/// The on-disk form of a Storage object (localStorage): one JSON object whose
/// every value is a string, `{ "key": "value", ... }`, keys in map order.
///
/// It was already spelled that way, but by a hand escaper that knew four
/// escapes and a hand parser that knew the same four, so the file was JSON
/// only for values that happened to avoid the rest — a control character or a
/// `\u` sequence went out raw and came back wrong. These read and write real
/// JSON: every escape the grammar has, `\uXXXX` with surrogate pairs decoded
/// to UTF-8 on the way in and control characters encoded on the way out, and
/// a value that is not a string (a file edited by hand) is skipped rather
/// than truncating everything after it.
///
/// Both Storage implementations (QuickJS `js/storage_bindings.cpp`, bronze
/// `bronze_host/dom_storage.cpp`) go through here, so there is one format.

/// Read `path` into `out`. A missing or unreadable file leaves `out` empty and
/// returns false; a file that parses, even partially, returns true.
bool readStorageFile(const std::string& path, std::map<std::string, std::string>& out);

/// Write `items` to `path` atomically: the document is written whole to
/// `<path>.tmp` and renamed over `path`, so a crash mid-write leaves the
/// previous file intact rather than a truncated one — a Storage is rewritten
/// in full on every setItem, and there is no exit hook to flush at. Returns
/// false when the temp file could not be written or the rename failed.
bool writeStorageFile(const std::string& path, const std::map<std::string, std::string>& items);

/// `s` as a JSON string literal, quotes included.
std::string jsonQuote(const std::string& s);

} // namespace bro::util
