#pragma once

#include "bronze_host/host_values.h"
#include "eye/eye_types.h"

#include <span>

namespace bro::bronze_host::eye_detail {

bronze::Value perceptualDiffToJs(const eye::EyePerceptualDiff& d);
bronze::Value composeToJs(const eye::EyeComposeResult& r);
bronze::Value formToJs(const eye::EyeFormResult& r);

bronze::Value jsEyeCompose(bronze::Value, std::span<const bronze::Value> a);
bronze::Value jsEyeForm(bronze::Value, std::span<const bronze::Value> a);

#if BRO_WITH_3D
bool trySceneTasteVisualize(bronze::Value src,
                            const eye::EyeVisualizeOptions& vopts,
                            eye::EyeVisualResult& outResult,
                            std::string& outErr);
#endif

}  // namespace bro::bronze_host::eye_detail
