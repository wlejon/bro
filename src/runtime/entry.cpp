// libbro's exported entry points (bro_entry.h): the C functions every bro
// executable calls. The drivers behind them are runtime_mains.h's;
// bro_run_app is app_exe_main.cpp's.

#include "runtime/bro_entry.h"

#include "engine/app_exe.h"
#include "runtime/runtime_mains.h"

extern "C" {

BRO_API int bro_run_windowed(int argc, char** argv) { return bro::runtime::runWindowed(argc, argv, nullptr); }

BRO_API int bro_run_headless(int argc, char** argv) { return bro::runtime::runHeadlessMain(argc, argv); }

BRO_API int bro_run_server(int argc, char** argv) { return bro::runtime::runServerMain(argc, argv); }

BRO_API uint32_t bro_entry_abi(void) { return BRO_ENTRY_ABI; }

BRO_API const char* bro_engine_version(void) { return bro::engine::engineVersion().c_str(); }

}  // extern "C"
