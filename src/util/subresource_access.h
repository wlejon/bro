#pragma once

// Whether a document may read a file one of its subresources names (an <img>
// or CSS url(), a font, a <video>, an <iframe> folder): the host's access gate
// (bronze's embed/embed_access.h), as bro's layers below the bronze host ask
// it. The bronze host installs the forwarder (bronze_host/host_access.cpp);
// until it does, and in a process without one, every read is allowed.
//
// A subresource load never asks the user: it is answered from what is
// decided (checkAccessWithoutAsking), so markup cannot raise a prompt, and a
// capability not yet decided is a refusal.

#include <string>

namespace bro::util {

/// Answers "" to allow, or the refusal's sentence, for reading the absolute,
/// lexically normal path `path`.
using SubresourceReadGate = std::string (*)(const std::string& path);

void setSubresourceReadGate(SubresourceReadGate gate);

/// The gate's answer for reading `path` (made absolute and lexically normal
/// here): "" when allowed or when no gate is installed.
std::string subresourceReadRefusal(const std::string& path);

}  // namespace bro::util
