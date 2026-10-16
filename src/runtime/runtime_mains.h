#pragma once

// The three drivers inside libbro, behind entry.cpp's C entry points
// (bro_entry.h): the windowed run loop, the headless script driver and the
// server tick loop. They were each executable's main(); they live in the
// library so that the executables are one-line callers and every bro process
// maps one copy of the engine.

#include <string>

namespace bro::runtime {

/// What an installed app's own executable tells the windowed driver
/// (app_exe.cpp settles it from the identity stamped into the executable).
struct AppExeLaunch {
    std::string appDir;   // the app folder, absolute
    std::string exePath;  // the executable itself, absolute
};

/// `bro [flags] <app> [args]`, or with `exe` an app executable's launch:
/// the target is exe->appDir and every argument is the app's.
int runWindowed(int argc, char* argv[], const AppExeLaunch* exe);

int runHeadlessMain(int argc, char* argv[]);
int runServerMain(int argc, char* argv[]);

}  // namespace bro::runtime
