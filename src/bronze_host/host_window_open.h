#pragma once

#include "bronze_host/host_realm_scope.h"
#include "embed/embed.h"
#include <cstdint>
#include <span>
namespace bro::engine { class Engine; }

namespace bro::bronze_host {

struct Message;

void installBroWindowOpen(bronze::Value broWin);
void installBroWindowParent(bronze::Value broWin);

void windowHostNotifyLoaded(uint64_t id);
void windowHostNotifyClosed(uint64_t id);
void windowHostNotifyResized(uint64_t id, int width, int height);
void windowHostNotifyMessage(uint64_t id, bronze::Value data);

void addWindowHostChildMessageListener(uint64_t hostId, bronze::Value fn);
void removeWindowHostChildMessageListener(uint64_t hostId, bronze::Value fn);

void drainHostWindowMessages();

bronze::Value handleWindowOpen(std::span<const bronze::Value> args);

} // namespace bro::bronze_host
