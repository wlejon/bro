// bro-headless — the stock headless driver, with no host additions.
//
// The driver itself lives in the engine (engine/headless_driver.cpp) so that
// an application embedding bro_engine gets the same scripting, screenshot and
// assertion surface with its own bindings installed, instead of reimplementing
// it. This binary is that driver with an empty hook set.

#include "engine/headless_driver.h"

int main(int argc, char* argv[]) {
    return bro::engine::runHeadless(argc, argv);
}
