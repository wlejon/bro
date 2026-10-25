#pragma once
// The scene's shader sets compiled off the frame thread, and warmed up from
// the last run (docs/code-cache.md "Shader warm-up").
//
// Every world shader set (a ModuleShaders set: the built-in shaders with a
// world's modules in its files' places) is asked for here and compiled on a
// small pool of worker threads, one shader a job, each through the SPIR-V
// cache. One set asked for twice is compiled once: a request returns the
// future of the one already under way. A node's drawer asks when it first
// sees the node and draws nothing of it until the set is in
// (ModuleShaders::poll), so no frame waits on a compile.
//
// Each run records the sets it asked for, in the order asked, in the app's
// cache dir (shader-warmup.bin). The next run of the app reads the record at
// boot (begin) and asks for them all before the page's script has run, so
// the compiles a rebuild leaves to do (every SPIR-V cache entry missed) run
// in parallel with the page's own compile, and the nodes that ask later find
// them made or under way. The record is written again from what this run
// asks for, so a set the app no longer uses drops out of it.

#include "scene/vulkan/frame/scene_vk_shader_compiler.h"

#include <cstdint>
#include <future>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace bro::scene::vk::shader_warmup {

using Overrides = SceneVkShaderCompiler::IncludeOverrides;

/// A set's SPIR-V, one blob a stage, and each stage's diagnostics (an
/// empty blob is a stage that did not compile).
struct SetResult {
    std::vector<std::vector<uint32_t>> spirv;
    std::vector<std::string> errs;
};
using SetFuture = std::shared_future<std::shared_ptr<const SetResult>>;

/// The set of `stages` with `overrides`: the one under way or made, else a
/// new one queued on the workers. Recorded for the next run.
SetFuture request(std::span<const BuiltinSceneShader> stages, const Overrides& overrides);

/// Read the record at `recordPath` (the app's cache dir's
/// shader-warmup.bin) and queue every set in it; later requests are recorded
/// there. Once a process; "" records nothing. BRO_SHADER_WARMUP=0 neither
/// reads nor writes a record.
void begin(const std::string& recordPath);

/// Whether a frame skips what is not yet compiled (true) or waits for it
/// (false). The engine turns it on for a window (no frame waits, so the
/// window never stops answering) and leaves it off in headless, whose
/// renders are read back at once (tests, stills) and must hold everything.
/// BRO_ASYNC_SHADERS=1 / 0 overrides either: a headless run measuring boot
/// frames as a window has them sets 1.
void setAsync(bool on);
bool async();

struct Progress {
    int sets = 0;        // sets asked for in this process (warm-up included)
    int setsDone = 0;
    int shaders = 0;     // their shaders
    int shadersDone = 0;
    int warmupSets = 0;  // of the sets, those the record queued
};
Progress progress();

}  // namespace bro::scene::vk::shader_warmup
