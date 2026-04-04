#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bro::canvas {

enum class CmdType : uint8_t {
    SetFillStyle, SetStrokeStyle, SetLineWidth, SetGlobalAlpha, SetFont,
    FillRect, StrokeRect, ClearRect, FillText,
    Save, Restore, Translate, Rotate, Scale,
    BeginPath, MoveTo, LineTo, ClosePath, Stroke, Fill,
    Arc, // x, y, radius(w), startAngle(h), endAngle(f); uses extra bool for anticlockwise
};

struct DrawCmd {
    CmdType type;
    float x = 0, y = 0, w = 0, h = 0;
    uint8_t r = 0, g = 0, b = 0, a = 255;
    float f = 0;           // lineWidth, globalAlpha, angle
    std::string text;      // for FillText, SetFont
};

struct Canvas2DState {
    uint8_t fillR = 0, fillG = 0, fillB = 0, fillA = 255;
    uint8_t strokeR = 0, strokeG = 0, strokeB = 0, strokeA = 255;
    float lineWidth = 1.0f;
    float globalAlpha = 1.0f;
    std::string font = "16px sans-serif";
};

class Canvas2D {
public:
    void setFillStyle(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        state_.fillR = r; state_.fillG = g; state_.fillB = b; state_.fillA = a;
        DrawCmd c; c.type = CmdType::SetFillStyle; c.r = r; c.g = g; c.b = b; c.a = a;
        cmds_.push_back(c);
    }
    void setStrokeStyle(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        state_.strokeR = r; state_.strokeG = g; state_.strokeB = b; state_.strokeA = a;
        DrawCmd c; c.type = CmdType::SetStrokeStyle; c.r = r; c.g = g; c.b = b; c.a = a;
        cmds_.push_back(c);
    }
    void setLineWidth(float w) {
        state_.lineWidth = w;
        DrawCmd c; c.type = CmdType::SetLineWidth; c.f = w;
        cmds_.push_back(c);
    }
    void setGlobalAlpha(float a) {
        state_.globalAlpha = a;
        DrawCmd c; c.type = CmdType::SetGlobalAlpha; c.f = a;
        cmds_.push_back(c);
    }
    void setFont(const std::string& font) {
        state_.font = font;
        DrawCmd c; c.type = CmdType::SetFont; c.text = font;
        cmds_.push_back(c);
    }

    void fillRect(float x, float y, float w, float h) {
        DrawCmd c; c.type = CmdType::FillRect; c.x = x; c.y = y; c.w = w; c.h = h;
        cmds_.push_back(c);
    }
    void strokeRect(float x, float y, float w, float h) {
        DrawCmd c; c.type = CmdType::StrokeRect; c.x = x; c.y = y; c.w = w; c.h = h;
        cmds_.push_back(c);
    }
    void clearRect(float x, float y, float w, float h, int canvasW, int canvasH) {
        // Full-canvas clear resets the command buffer
        if (x <= 0 && y <= 0 && w >= canvasW && h >= canvasH) {
            cmds_.clear();
        }
        DrawCmd c; c.type = CmdType::ClearRect; c.x = x; c.y = y; c.w = w; c.h = h;
        cmds_.push_back(c);
    }
    void fillText(const std::string& text, float x, float y) {
        DrawCmd c; c.type = CmdType::FillText; c.text = text; c.x = x; c.y = y;
        cmds_.push_back(c);
    }

    void save() { stateStack_.push_back(state_); DrawCmd c; c.type = CmdType::Save; cmds_.push_back(c); }
    void restore() {
        if (!stateStack_.empty()) { state_ = stateStack_.back(); stateStack_.pop_back(); }
        DrawCmd c; c.type = CmdType::Restore; cmds_.push_back(c);
    }
    void translate(float tx, float ty) { DrawCmd c; c.type = CmdType::Translate; c.x = tx; c.y = ty; cmds_.push_back(c); }
    void rotate(float angle) { DrawCmd c; c.type = CmdType::Rotate; c.f = angle; cmds_.push_back(c); }
    void scale(float sx, float sy) { DrawCmd c; c.type = CmdType::Scale; c.x = sx; c.y = sy; cmds_.push_back(c); }

    // Path API
    void beginPath() { DrawCmd c; c.type = CmdType::BeginPath; cmds_.push_back(c); }
    void moveTo(float x, float y) { DrawCmd c; c.type = CmdType::MoveTo; c.x = x; c.y = y; cmds_.push_back(c); }
    void lineTo(float x, float y) { DrawCmd c; c.type = CmdType::LineTo; c.x = x; c.y = y; cmds_.push_back(c); }
    void closePath() { DrawCmd c; c.type = CmdType::ClosePath; cmds_.push_back(c); }
    void stroke() { DrawCmd c; c.type = CmdType::Stroke; cmds_.push_back(c); }
    void fill() { DrawCmd c; c.type = CmdType::Fill; cmds_.push_back(c); }
    void arc(float cx, float cy, float radius, float startAngle, float endAngle, bool anticlockwise = false) {
        DrawCmd c; c.type = CmdType::Arc; c.x = cx; c.y = cy; c.w = radius;
        c.h = startAngle; c.f = endAngle;
        c.r = anticlockwise ? 1 : 0; // reuse r field for bool
        cmds_.push_back(c);
    }

    const std::vector<DrawCmd>& commands() const { return cmds_; }
    const Canvas2DState& state() const { return state_; }

    // Discard all queued commands without drawing anything (used to hide the scene)
    void reset() { cmds_.clear(); }

private:
    std::vector<DrawCmd> cmds_;
    Canvas2DState state_;
    std::vector<Canvas2DState> stateStack_;
};

// Parse CSS color: "#rgb", "#rrggbb", "rgb(r,g,b)", "rgba(r,g,b,a)", named colors
bool parseCSSColor(const std::string& str, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a);

// Parse CSS font: "16px Arial", "bold 20px monospace"
struct ParsedFont { std::string family; float size; int weight; bool italic; };
ParsedFont parseCSSFont(const std::string& font);

} // namespace bro::canvas
