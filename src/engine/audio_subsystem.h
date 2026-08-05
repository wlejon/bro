#pragma once

typedef struct JSContext JSContext;

namespace bro::engine {

void tickAudioInferenceSubsystem(JSContext* ctx);
void tickMicSubsystem(JSContext* ctx);

} // namespace bro::engine
