#pragma once
#include "embed/embed.h"
#include "dom/event.h"
#include "bronze_host/host_internal.h"
#include <vector>

namespace bro::bronze_host {

extern HostClass g_touchClass;
extern HostClass g_touchListClass;
extern HostClass g_touchEventClass;
extern HostClass g_gestureEventClass;

void installTouchGlobals();

Value makeTouchValue(const dom::TouchPoint& pt);
Value makeTouchListValue(const std::vector<dom::TouchPoint>& points);

} // namespace bro::bronze_host
