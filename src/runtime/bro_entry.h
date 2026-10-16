#pragma once

/* The engine library's entry points: the whole surface an executable needs to
 * run bro.
 *
 * bro's engine is one shared library, libbro (libbro.so, libbro.dll,
 * libbro.dylib). `bro`, `bro-headless`, `bro-server` and every installed
 * app's own executable are a few lines each that call one of these, so every
 * running bro process maps the same copy of the engine's code.
 *
 * This is a C ABI on purpose. It is what crosses the library boundary on
 * every platform, and on Windows it is ALL that crosses it: the DLL exports
 * these functions and nothing else, so the engine's C++ surface (and the
 * sibling libraries linked into it) never has to be annotated for export. On
 * ELF and Mach-O the C++ surface is exported as well (default visibility),
 * which is what an embedder that drives the Engine from C++ (helm) links
 * against there. docs/apps.md and BUILDING.md ("The engine library") have
 * the reasoning.
 *
 * BRO_ENTRY_ABI changes only when a signature or the meaning of an argument
 * below changes. An app executable checks it before calling in, and runs the
 * app as a folder under the stock `bro` when it does not match. */

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(BRO_RUNTIME_BUILD)
#    define BRO_API __declspec(dllexport)
#  elif defined(BRO_RUNTIME_SHARED)
#    define BRO_API __declspec(dllimport)
#  else
#    define BRO_API
#  endif
#else
#  define BRO_API __attribute__((visibility("default")))
#endif

#define BRO_ENTRY_ABI 1u

#ifdef __cplusplus
extern "C" {
#endif

/* The stock executables' main()s, verbatim: argc/argv as main() received
 * them. On Windows the command line is re-read from GetCommandLineW (argv is
 * in the ANSI code page there and cannot carry every path).
 *
 * (Named bro_run_* because bro_<name>_main is the entry symbol of bro's
 * build-time compiled JS modules, src/bronze_host/js/<name>.js.) */
BRO_API int bro_run_windowed(int argc, char** argv);
BRO_API int bro_run_headless(int argc, char** argv);
BRO_API int bro_run_server(int argc, char** argv);

/* An installed app's own executable (docs/apps.md, "The app's executable").
 * `identity` is the block `bro --install` stamped into it, `identityLen` its
 * length in bytes: the app's id, name, version, folder and the engine and
 * content hash it was built against, as `key=value` lines. Every command-line
 * argument is the app's (bro.app.argv), apart from bro's own few flags before
 * a `--`. An executable built against another engine build, or for an app
 * whose manifest or icon changed since, is out of date: the app runs as a
 * folder under the stock `bro` beside this library while a fresh executable
 * is built, so it always starts. */
BRO_API int bro_run_app(int argc, char** argv, const char* identity, size_t identityLen);

/* BRO_ENTRY_ABI of the library. */
BRO_API uint32_t bro_entry_abi(void);

/* This engine build's version: `<bro version>+<commit>[-dirty].<build stamp>`.
 * It changes with every link of the library; an app executable records the
 * one it was built against. */
BRO_API const char* bro_engine_version(void);

#ifdef __cplusplus
}
#endif
