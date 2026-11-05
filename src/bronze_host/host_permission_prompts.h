#pragma once

// bro.permissions' prompt requests (docs/permissions-api.js): the shell's end
// of platform/permission_prompts.h. The shell serves the session's prompts
// once something subscribes to 'request', and stops when nothing does; each
// request is an object the shell answers with respond(true | false), and an
// asker that goes away first is a 'cancel'. Main thread.

#include "embed/embed.h"

#include <string>

namespace bro::bronze_host {

/// bro.permissions.on(type, fn) for 'request' and 'cancel': false when
/// `type` is neither (the caller handles 'change'). The first 'request'
/// listener starts serving.
bool addPromptListener(const std::string& type, bronze::Value fn);
/// bro.permissions.off(type, fn): the last 'request' listener gone stops
/// serving, and every outstanding request is cancelled.
bool removePromptListener(const std::string& type, bronze::Value fn);
/// Whether this process serves the prompts now (bro.permissions.serving).
bool servingPermissionPrompts();
/// Deliver what arrived since the last call; once a frame.
void pumpPermissionPrompts();
/// Engine shutdown: stop serving.
void stopPermissionPrompts();

}  // namespace bro::bronze_host
