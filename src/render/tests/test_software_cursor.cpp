#include "render/software_cursor.h"

#include <include/core/SkBitmap.h>
#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkSurface.h>

#include <iostream>
#include <string>
#include <vector>

namespace {

int gFailures = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::cerr << "  FAIL: " #cond " (line " << __LINE__ << ")" << std::endl;  \
            ++gFailures;                                                              \
        }                                                                             \
    } while (0)

uint32_t countNonZeroPixels(SkSurface* surface) {
    if (!surface) return 0;
    SkPixmap pixmap;
    if (!surface->peekPixels(&pixmap)) return 0;
    uint32_t count = 0;
    for (int y = 0; y < pixmap.height(); ++y) {
        for (int x = 0; x < pixmap.width(); ++x) {
            if (SkColorGetA(pixmap.getColor(x, y)) > 0) {
                ++count;
            }
        }
    }
    return count;
}

} // namespace

int main() {
    std::cout << "=== Running test_software_cursor ===" << std::endl;

    // 1. Null canvas safety
    bro::render::drawSoftwareCursor(nullptr, 10.0f, 10.0f, "default");
    CHECK(true);

    // 2. Shape "none" does not draw
    {
        auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(64, 64));
        surface->getCanvas()->clear(SK_ColorTRANSPARENT);
        bro::render::drawSoftwareCursor(surface->getCanvas(), 32.0f, 32.0f, "none");
        CHECK(countNonZeroPixels(surface.get()) == 0);
    }

    // 3. Arrow cursor ("default") draws visible pixels
    {
        auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(64, 64));
        surface->getCanvas()->clear(SK_ColorTRANSPARENT);
        bro::render::drawSoftwareCursor(surface->getCanvas(), 32.0f, 32.0f, "default");
        uint32_t painted = countNonZeroPixels(surface.get());
        CHECK(painted > 50);
        std::cout << "  'default' arrow drew " << painted << " pixels" << std::endl;
    }

    // 4. Test all standard shapes
    const std::vector<std::string> shapes = {
        "pointer", "hand", "text", "vertical-text", "xterm",
        "crosshair", "cell", "move", "ew-resize", "col-resize",
        "ns-resize", "row-resize", "not-allowed", "no-drop", "wait", "progress"
    };

    for (const auto& shape : shapes) {
        auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(64, 64));
        surface->getCanvas()->clear(SK_ColorTRANSPARENT);
        bro::render::drawSoftwareCursor(surface->getCanvas(), 32.0f, 32.0f, shape, 1.0f);
        uint32_t painted = countNonZeroPixels(surface.get());
        CHECK(painted > 20);
        std::cout << "  '" << shape << "' cursor drew " << painted << " pixels" << std::endl;
    }

    if (gFailures == 0) {
        std::cout << "=== test_software_cursor: ALL TESTS PASSED ===" << std::endl;
        return 0;
    } else {
        std::cerr << "=== test_software_cursor: " << gFailures << " FAILURES ===" << std::endl;
        return 1;
    }
}
