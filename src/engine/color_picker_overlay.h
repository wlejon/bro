#pragma once

#include "engine/overlay.h"
#include <array>
#include <functional>
#include <string>

namespace bro::engine {

/// Full-spectrum color picker overlay. Provides:
///   - Saturation/value square
///   - Vertical hue slider
///   - Horizontal alpha slider (checkerboard backing)
///   - Editable hex input (#rgb, #rrggbb, #rrggbbaa all accepted)
///   - R / G / B / A numeric readouts
///   - Preset swatch row
///
/// Internally represents color as HSVA; the callback receives a hex string.
/// If the element supports alpha the hex is 8-char (#rrggbbaa); otherwise
/// it's 6-char (#rrggbb).
class ColorPickerOverlay : public Overlay {
public:
    /// Callback fired on every change (drag, hex edit, preset). The argument
    /// is the current color as a hex string of length 7 or 9 (including #).
    /// Maps to the "input" DOM event.
    using ChangeCallback = std::function<void(const std::string& hex)>;

    /// Callback fired once when the picker is dismissed. Receives the final
    /// color. Maps to the "change" DOM event.
    using CommitCallback = std::function<void(const std::string& hex)>;

    /// @param anchorX,anchorY,anchorW,anchorH: screen-space rect of the
    ///        input swatch the picker is anchored to. The popup opens below.
    /// @param initialHex: initial color — #rgb, #rgba, #rrggbb, #rrggbbaa.
    /// @param withAlpha: true if the picker should expose alpha channel.
    ColorPickerOverlay(float anchorX, float anchorY, float anchorW, float anchorH,
                       float viewportW, float viewportH,
                       const std::string& initialHex,
                       bool hasAlpha,
                       ChangeCallback onChange,
                       CommitCallback onCommit = {});

    void onDismiss() override;

    void getBounds(float& x, float& y, float& w, float& h) const override;
    void draw(render::Renderer* r) override;
    bool onMouseDown(float x, float y, int button) override;
    bool onMouseMove(float x, float y) override;
    bool onMouseUp(float x, float y, int button) override;
    bool onKeyDown(int keycode, int mod) override;
    bool onTextInput(const std::string& text) override;

private:
    // HSVA state (H in 0..360, S/V/A in 0..1).
    float h_ = 0.0f;
    float s_ = 0.0f;
    float v_ = 0.0f;
    float a_ = 1.0f;
    bool hasAlpha_;

    ChangeCallback onChange_;
    CommitCallback onCommit_;
    std::string lastEmitted_; // dedupe live onChange dispatches

    // Popup position (top-left of the overlay box, screen space)
    float originX_;
    float originY_;
    static constexpr float kPopupW = 260.0f;
    static constexpr float kPopupH = 344.0f;

    // Region geometry (relative to originX/Y, computed in initRects()).
    struct Rect { float x, y, w, h; };
    Rect rSV_{};
    Rect rHue_{};
    Rect rAlpha_{};
    Rect rPreview_{};
    Rect rHex_{};
    Rect rRGBRow_{};
    std::array<Rect, 8> rPresets_{};

    // Drag mode — which region a mouse-down captured.
    enum class Drag { None, SV, Hue, Alpha } drag_ = Drag::None;

    // Hex text field state
    std::string hexText_;
    int hexCursor_ = 0;
    bool hexFocused_ = false;

    // Cached line metrics (resolved on first paint).
    bool fontMetricsResolved_ = false;
    float fontAscent_ = 0.0f;
    float fontSmallAscent_ = 0.0f;

    // --- Internals ---

    void initRects();

    /// Apply text-field edits: parse hex, update HSVA if valid.
    void applyHexText();

    /// Emit onChange with the current HSVA as a hex string.
    void emitChange();

    /// Regenerate hexText_ from current HSVA.
    void syncHexText();

    /// Current HSVA formatted as "#rrggbb" or "#rrggbbaa" depending on hasAlpha_.
    std::string formatHex() const;

    /// Parse a CSS color string into HSVA (delegates to DrawTraversal::tryParseColor).
    /// Accepts any CSS color syntax: hex, rgb/rgba, hsl/hsla, named colors.
    static bool parseColor(const std::string& s,
                           float& outH, float& outS, float& outV, float& outA);

    static void hsvToRgb(float h, float s, float v,
                         uint8_t& r, uint8_t& g, uint8_t& b);
    static void rgbToHsv(uint8_t r, uint8_t g, uint8_t b,
                         float& h, float& s, float& v);

    // Preset colors (8 fixed swatches), resolved once in the constructor.
    static const char* const kPresetHex[8];
    std::array<render::Color, 8> presetColors_{};
};

} // namespace bro::engine
