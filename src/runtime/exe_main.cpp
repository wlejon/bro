// main() of bro, bro-headless and bro-server: one line into the engine
// library (bro_entry.h). BRO_EXE_ENTRY names which entry point this build of
// the file calls.

#include "runtime/bro_entry.h"

int main(int argc, char** argv) { return BRO_EXE_ENTRY(argc, argv); }
