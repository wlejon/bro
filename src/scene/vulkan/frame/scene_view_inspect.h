#pragma once

// What changed on the world between two renders, measured rather than seen.
// A render's depth and colour (read back with it, SceneVkBridge::setInspect)
// and its camera put every pixel at a point on the body; each pixel of a
// render is carried to where that point lands in the render before, and the
// two compared there:
//
//   same      the previous render saw the same surface at that spot (within
//             a couple of pixel footprints): its normal and its luma are
//             compared, so a surface that changed shape (a morph, a re-tiled
//             ground) or look (clouds that vanish over it) shows;
//   rose      something now stands in front of what the previous render saw
//             (ground or trees that grew into the view);
//   sank      the previous render's surface is gone from in front (ground
//             that fell away, trees that vanished; parallax disocclusion
//             too, which is thin along edges);
//   appeared  a surface where the previous render saw sky;
//   vanished  sky where the previous render saw a surface;
//   new view  a point outside the previous render's view.
//
// The camera's own turn, roll and travel between the two renders come with
// it. Positions are on the body (place origin + basis, scene/world/place.h),
// so a re-anchor of the place frame between two renders changes nothing.

#include <bromath/mat.h>
#include <bromath/vec.h>

#include <cstdint>
#include <vector>

namespace bro::scene::vk {

/// One render as the inspector reads it.
struct InspectCamera {
    bromath::Mat4 invProj;       // Vulkan clip -> view
    bromath::Mat4 relView;       // camera-relative place point -> view
    bromath::Mat4 relViewProj;   // camera-relative place point -> Vulkan clip
    double eye[3] = {0.0, 0.0, 0.0};      // in the place frame
    double origin[3] = {0.0, 0.0, 0.0};   // the place frame on the body
    double basis[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};   // row-major, column j = axis j
};

class ViewInspector {
public:
    enum class Image { None, Change, Normals };

    /// The per-frame numbers, in this order (inspectFrame's array).
    enum Stat {
        kWidth, kHeight,
        kCompared,      // 1 when a previous render was compared
        kSame, kRose, kSank, kAppeared, kVanished, kNewView, kSky,   // fractions of the pixels
        kNormalDeg,     // mean normal change (degrees) over `same`
        kNormalMoved,   // fraction of `same` whose normal turned past 15 degrees
        kLuma,          // mean luma change over `same`
        kLumaMoved,     // fraction of `same` whose luma changed past 0.08
        kSkyLuma,       // mean luma change over sky seen as sky in both
        kTurnDeg,       // the camera's forward turned (degrees)
        kRollDeg,       // its up turned about the forward (degrees, signed)
        kTravel,        // the eye moved (m)
        kMedianDist,    // the median surface distance (m), 0 when all sky
        kNearDist,      // the surfaces' tenth nearest (m): a tenth of them nearer, 0 when all sky
        kLumaSpread,    // the frame's luma's standard deviation (0..0.5): near 0 a blank view (inside a cloud, a fog)
        kBands,        // then 4 bands, top to bottom: moved (rose+sank+appeared+vanished),
                        // normalMoved and lumaMoved, each a fraction of the band's pixels
        kStatCount = kBands + 12,
    };

    /// Compare this render with the last one given. `depth` is reversed-Z
    /// (0 = far), `rgba` top-down; both `width` x `height`. Fills `stats`
    /// (kStatCount) and, unless `image` is None, `outImage` (RGBA).
    void inspect(const InspectCamera& cam, uint32_t width, uint32_t height, const float* depth,
                 const uint8_t* rgba, Image image, std::vector<double>& stats,
                 std::vector<uint8_t>& outImage);

    /// Forget the previous render (the next one compares with nothing).
    void reset() { prevW_ = prevH_ = 0; }

private:
    struct Frame {
        uint32_t w = 0, h = 0;
        double eyeBody[3] = {0, 0, 0};
        double basis[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        bromath::Mat4 relViewProj;
        double right[3] = {1, 0, 0}, up[3] = {0, 1, 0}, fwd[3] = {0, 0, -1};   // body axes
        std::vector<bromath::Vec3> off;     // the pixel's point less the eye, body axes
        std::vector<bromath::Vec3> normal;  // body axes; zero where unknown
        std::vector<float> luma;
        std::vector<uint8_t> sky;
    };

    void build(const InspectCamera& cam, uint32_t width, uint32_t height, const float* depth,
               const uint8_t* rgba, Frame& f);
    // The previous render's pixel of a body point (false outside its view).
    bool projectPrev(const double bodyPoint[3], bool direction, int& px, int& py) const;
    double lumaChange(float luma, int px, int py) const;
    double normalChange(const bromath::Vec3& normal, int px, int py) const;

    Frame cur_, prev_;
    uint32_t prevW_ = 0, prevH_ = 0;
    double prevPix_ = 0.0;   // the previous render's pixel angle
};

}  // namespace bro::scene::vk
