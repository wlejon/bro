#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace bro::render {

enum class ShaderStage { Vertex, Fragment, Compute };

/// Compile Vulkan-flavoured GLSL (`#version 450` with explicit layouts) to
/// SPIR-V 1.0 for a Vulkan 1.0 client, in process, with glslang — the one
/// shader compiler bro has. The scene's built-in shaders go through it at
/// build time (bro_spirv_embed), and the worlds' modules and custom scene
/// shaders at run time; WebGL programs use glslang directly (acquireGlslang)
/// because linking them needs its reflection.
///
/// Returns the SPIR-V words, or an empty vector with glslang's diagnostics in
/// `*log` (when given) if the source does not compile. Successful results are
/// memoised by (stage, source), since the same variant is often requested
/// again (a material rebuilt, a terrain recreated). Thread-safe, and compiles
/// on several threads at once run in parallel.
std::vector<uint32_t> compileGlslToSpirv(std::string_view source, ShaderStage stage,
                                         std::string* log = nullptr);

/// The source bro compiles for a GLSL file on disk: the file with each line
/// `#include "name"` replaced by the file `name` (recursively), and each of
/// `defines` made `#define NAME 1` right after the #version line, so one file
/// builds several variants. Every `name`, in the file and in what it
/// includes, is a path from `includeDir` (a directory, with or without its
/// trailing slash), or from the file's own directory when `includeDir` is
/// empty. bro_spirv_embed compiles the scene's built-in shaders from exactly
/// this text at build time, and the scene does at run time under
/// BRO_SCENE_SHADER_SOURCE, so the two agree word for word. `deps`, when
/// given, receives every file read (the input first). False, with the reason
/// in `*err`, when a file cannot be read or the text has no #version line to
/// define after.
bool preprocessGlslFile(const std::string& path, const std::vector<std::string>& defines,
                        std::string& out, std::vector<std::string>* deps = nullptr,
                        std::string* err = nullptr, const std::string& includeDir = {});

/// For code that drives glslang's C++ API itself (WebGL's program linker
/// needs its reflection): the lock compileGlslToSpirv holds, taken with the
/// process initialised. glslang may be used for as long as the returned lock
/// is held; the lock is empty (owns_lock() false) if initialisation failed.
std::unique_lock<std::mutex> acquireGlslang();

} // namespace bro::render
