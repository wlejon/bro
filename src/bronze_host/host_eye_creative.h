#pragma once

#include "bronze_host/host_values.h"

#include <span>

namespace bro::bronze_host::eye_detail {

bronze::Value jsEyeMotion(bronze::Value, std::span<const bronze::Value> a);
bronze::Value jsEyeProbe(bronze::Value, std::span<const bronze::Value> a);
bronze::Value jsEyeClearance(bronze::Value, std::span<const bronze::Value> a);
bronze::Value jsEyeStructure(bronze::Value, std::span<const bronze::Value> a);
bronze::Value jsEyeCloseup(bronze::Value, std::span<const bronze::Value> a);
bronze::Value jsEyeLookdev(bronze::Value, std::span<const bronze::Value> a);
bronze::Value jsEyeSweep(bronze::Value, std::span<const bronze::Value> a);
bronze::Value jsEyeTrace(bronze::Value, std::span<const bronze::Value> a);
bronze::Value jsEyeDiagnose(bronze::Value, std::span<const bronze::Value> a);
bronze::Value jsEyeConformance(bronze::Value, std::span<const bronze::Value> a);

}  // namespace bro::bronze_host::eye_detail
