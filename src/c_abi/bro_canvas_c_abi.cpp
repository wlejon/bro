// =============================================================================
// bro_canvas_c_abi.cpp — C++ forwarding implementations for bro.canvas
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_canvas_c_abi.h"
#include <cstdint>
#include <string>
#include <vector>
#include <utility>

struct BroCanvasGradientImpl {
    std::vector<std::pair<double, std::string>> stops;
};

struct BroTextMetricsImpl {
    double width = 0.0;
};

struct BroCanvasRenderingContext2DImpl {
    int32_t width = 300;
    int32_t height = 150;
    std::string fillStyle = "#000000";
    std::string strokeStyle = "#000000";
    double lineWidth = 1.0;
    std::string lineCap = "butt";
    std::string lineJoin = "miter";
    double miterLimit = 10.0;
    double globalAlpha = 1.0;
    std::string globalCompositeOperation = "source-over";
    double shadowOffsetX = 0.0;
    double shadowOffsetY = 0.0;
    double shadowBlur = 0.0;
    std::string shadowColor = "rgba(0,0,0,0)";
    std::string font = "10px sans-serif";
    std::string textAlign = "start";
    std::string textBaseline = "alphabetic";
    std::string direction = "inherit";
    bool imageSmoothingEnabled = true;
    std::string imageSmoothingQuality = "low";
    double lineDashOffset = 0.0;
};

extern "C" {

// CanvasGradient
void* bro_CanvasGradient_create(void) {
    return new BroCanvasGradientImpl();
}

void bro_CanvasGradient_destroy(void* self) {
    delete static_cast<BroCanvasGradientImpl*>(self);
}

void bro_CanvasGradient_addColorStop(void* self, double offset, const char* color) {
    auto* g = static_cast<BroCanvasGradientImpl*>(self);
    if (g && color) {
        g->stops.emplace_back(offset, std::string(color));
    }
}

// TextMetrics
void* bro_TextMetrics_create(void) {
    return new BroTextMetricsImpl();
}

void bro_TextMetrics_destroy(void* self) {
    delete static_cast<BroTextMetricsImpl*>(self);
}

double bro_TextMetrics_get_width(void* self) {
    auto* tm = static_cast<BroTextMetricsImpl*>(self);
    return tm ? tm->width : 0.0;
}

// CanvasRenderingContext2D
void* bro_CanvasRenderingContext2D_create(void) {
    return new BroCanvasRenderingContext2DImpl();
}

void bro_CanvasRenderingContext2D_destroy(void* self) {
    delete static_cast<BroCanvasRenderingContext2DImpl*>(self);
}

int32_t bro_CanvasRenderingContext2D_get_canvasWidth(void* self) {
    auto* c = static_cast<BroCanvasRenderingContext2DImpl*>(self);
    return c ? c->width : 300;
}

int32_t bro_CanvasRenderingContext2D_get_canvasHeight(void* self) {
    auto* c = static_cast<BroCanvasRenderingContext2DImpl*>(self);
    return c ? c->height : 150;
}

void* bro_CanvasRenderingContext2D_get_fillStyle(void* /*self*/) {
    return nullptr;
}

void bro_CanvasRenderingContext2D_set_fillStyle(void* self, void* /*val*/) {
    (void)self;
}

void* bro_CanvasRenderingContext2D_get_strokeStyle(void* /*self*/) {
    return nullptr;
}

void bro_CanvasRenderingContext2D_set_strokeStyle(void* self, void* /*val*/) {
    (void)self;
}

double bro_CanvasRenderingContext2D_get_lineWidth(void* self) {
    auto* c = static_cast<BroCanvasRenderingContext2DImpl*>(self);
    return c ? c->lineWidth : 1.0;
}

void bro_CanvasRenderingContext2D_set_lineWidth(void* self, double val) {
    auto* c = static_cast<BroCanvasRenderingContext2DImpl*>(self);
    if (c) c->lineWidth = val;
}

const char* bro_CanvasRenderingContext2D_get_lineCap(void* self) {
    auto* c = static_cast<BroCanvasRenderingContext2DImpl*>(self);
    return c ? c->lineCap.c_str() : "butt";
}

void bro_CanvasRenderingContext2D_set_lineCap(void* self, const char* val) {
    auto* c = static_cast<BroCanvasRenderingContext2DImpl*>(self);
    if (c && val) c->lineCap = val;
}

const char* bro_CanvasRenderingContext2D_get_lineJoin(void* self) {
    auto* c = static_cast<BroCanvasRenderingContext2DImpl*>(self);
    return c ? c->lineJoin.c_str() : "miter";
}

void bro_CanvasRenderingContext2D_set_lineJoin(void* self, const char* val) {
    auto* c = static_cast<BroCanvasRenderingContext2DImpl*>(self);
    if (c && val) c->lineJoin = val;
}

double bro_CanvasRenderingContext2D_get_miterLimit(void* self) {
    auto* c = static_cast<BroCanvasRenderingContext2DImpl*>(self);
    return c ? c->miterLimit : 10.0;
}

void bro_CanvasRenderingContext2D_set_miterLimit(void* self, double val) {
    auto* c = static_cast<BroCanvasRenderingContext2DImpl*>(self);
    if (c) c->miterLimit = val;
}

double bro_CanvasRenderingContext2D_get_globalAlpha(void* self) {
    auto* c = static_cast<BroCanvasRenderingContext2DImpl*>(self);
    return c ? c->globalAlpha : 1.0;
}

void bro_CanvasRenderingContext2D_set_globalAlpha(void* self, double val) {
    auto* c = static_cast<BroCanvasRenderingContext2DImpl*>(self);
    if (c) c->globalAlpha = val;
}

const char* bro_CanvasRenderingContext2D_get_globalCompositeOperation(void* self) {
    auto* c = static_cast<BroCanvasRenderingContext2DImpl*>(self);
    return c ? c->globalCompositeOperation.c_str() : "source-over";
}

void bro_CanvasRenderingContext2D_set_globalCompositeOperation(void* self, const char* val) {
    auto* c = static_cast<BroCanvasRenderingContext2DImpl*>(self);
    if (c && val) c->globalCompositeOperation = val;
}

double bro_CanvasRenderingContext2D_get_shadowOffsetX(void* self) {
    auto* c = static_cast<BroCanvasRenderingContext2DImpl*>(self);
    return c ? c->shadowOffsetX : 0.0;
}

void bro_CanvasRenderingContext2D_set_shadowOffsetX(void* self, double val) {
    auto* c = static_cast<BroCanvasRenderingContext2DImpl*>(self);
    if (c) c->shadowOffsetX = val;
}

double bro_CanvasRenderingContext2D_get_shadowOffsetY(void* self) {
    auto* c = static_cast<BroCanvasRenderingContext2DImpl*>(self);
    return c ? c->shadowOffsetY : 0.0;
}

void bro_CanvasRenderingContext2D_set_shadowOffsetY(void* self, double val) {
    auto* c = static_cast<BroCanvasRenderingContext2DImpl*>(self);
    if (c) c->shadowOffsetY = val;
}

double bro_CanvasRenderingContext2D_get_shadowBlur(void* self) {
    auto* c = static_cast<BroCanvasRenderingContext2DImpl*>(self);
    return c ? c->shadowBlur : 0.0;
}

void bro_CanvasRenderingContext2D_set_shadowBlur(void* self, double val) {
    auto* c = static_cast<BroCanvasRenderingContext2DImpl*>(self);
    if (c) c->shadowBlur = val;
}

const char* bro_CanvasRenderingContext2D_get_shadowColor(void* self) {
    auto* c = static_cast<BroCanvasRenderingContext2DImpl*>(self);
    return c ? c->shadowColor.c_str() : "rgba(0,0,0,0)";
}

void bro_CanvasRenderingContext2D_set_shadowColor(void* self, const char* val) {
    auto* c = static_cast<BroCanvasRenderingContext2DImpl*>(self);
    if (c && val) c->shadowColor = val;
}

const char* bro_CanvasRenderingContext2D_get_font(void* self) {
    auto* c = static_cast<BroCanvasRenderingContext2DImpl*>(self);
    return c ? c->font.c_str() : "10px sans-serif";
}

void bro_CanvasRenderingContext2D_set_font(void* self, const char* val) {
    auto* c = static_cast<BroCanvasRenderingContext2DImpl*>(self);
    if (c && val) c->font = val;
}

const char* bro_CanvasRenderingContext2D_get_textAlign(void* self) {
    auto* c = static_cast<BroCanvasRenderingContext2DImpl*>(self);
    return c ? c->textAlign.c_str() : "start";
}

void bro_CanvasRenderingContext2D_set_textAlign(void* self, const char* val) {
    auto* c = static_cast<BroCanvasRenderingContext2DImpl*>(self);
    if (c && val) c->textAlign = val;
}

const char* bro_CanvasRenderingContext2D_get_textBaseline(void* self) {
    auto* c = static_cast<BroCanvasRenderingContext2DImpl*>(self);
    return c ? c->textBaseline.c_str() : "alphabetic";
}

void bro_CanvasRenderingContext2D_set_textBaseline(void* self, const char* val) {
    auto* c = static_cast<BroCanvasRenderingContext2DImpl*>(self);
    if (c && val) c->textBaseline = val;
}

const char* bro_CanvasRenderingContext2D_get_direction(void* self) {
    auto* c = static_cast<BroCanvasRenderingContext2DImpl*>(self);
    return c ? c->direction.c_str() : "inherit";
}

void bro_CanvasRenderingContext2D_set_direction(void* self, const char* val) {
    auto* c = static_cast<BroCanvasRenderingContext2DImpl*>(self);
    if (c && val) c->direction = val;
}

bool bro_CanvasRenderingContext2D_get_imageSmoothingEnabled(void* self) {
    auto* c = static_cast<BroCanvasRenderingContext2DImpl*>(self);
    return c ? c->imageSmoothingEnabled : true;
}

void bro_CanvasRenderingContext2D_set_imageSmoothingEnabled(void* self, bool val) {
    auto* c = static_cast<BroCanvasRenderingContext2DImpl*>(self);
    if (c) c->imageSmoothingEnabled = val;
}

const char* bro_CanvasRenderingContext2D_get_imageSmoothingQuality(void* self) {
    auto* c = static_cast<BroCanvasRenderingContext2DImpl*>(self);
    return c ? c->imageSmoothingQuality.c_str() : "low";
}

void bro_CanvasRenderingContext2D_set_imageSmoothingQuality(void* self, const char* val) {
    auto* c = static_cast<BroCanvasRenderingContext2DImpl*>(self);
    if (c && val) c->imageSmoothingQuality = val;
}

double bro_CanvasRenderingContext2D_get_lineDashOffset(void* self) {
    auto* c = static_cast<BroCanvasRenderingContext2DImpl*>(self);
    return c ? c->lineDashOffset : 0.0;
}

void bro_CanvasRenderingContext2D_set_lineDashOffset(void* self, double val) {
    auto* c = static_cast<BroCanvasRenderingContext2DImpl*>(self);
    if (c) c->lineDashOffset = val;
}

void bro_CanvasRenderingContext2D_save(void* /*self*/) {}
void bro_CanvasRenderingContext2D_restore(void* /*self*/) {}
void bro_CanvasRenderingContext2D_reset(void* /*self*/) {}
void bro_CanvasRenderingContext2D_beginPath(void* /*self*/) {}
void bro_CanvasRenderingContext2D_closePath(void* /*self*/) {}
void bro_CanvasRenderingContext2D_stroke(void* /*self*/) {}
void bro_CanvasRenderingContext2D_fill(void* /*self*/) {}
void bro_CanvasRenderingContext2D_clip(void* /*self*/) {}
void bro_CanvasRenderingContext2D_resetTransform(void* /*self*/) {}

void bro_CanvasRenderingContext2D_fillRect(void* /*self*/, double /*x*/, double /*y*/, double /*w*/, double /*h*/) {}
void bro_CanvasRenderingContext2D_strokeRect(void* /*self*/, double /*x*/, double /*y*/, double /*w*/, double /*h*/) {}
void bro_CanvasRenderingContext2D_clearRect(void* /*self*/, double /*x*/, double /*y*/, double /*w*/, double /*h*/) {}

void bro_CanvasRenderingContext2D_fillText(void* /*self*/, const char* /*text*/, double /*x*/, double /*y*/, double /*maxWidth*/) {}
void bro_CanvasRenderingContext2D_strokeText(void* /*self*/, const char* /*text*/, double /*x*/, double /*y*/, double /*maxWidth*/) {}

void bro_CanvasRenderingContext2D_translate(void* /*self*/, double /*x*/, double /*y*/) {}
void bro_CanvasRenderingContext2D_rotate(void* /*self*/, double /*angle*/) {}
void bro_CanvasRenderingContext2D_scale(void* /*self*/, double /*x*/, double /*y*/) {}
void bro_CanvasRenderingContext2D_setTransform(void* /*self*/, double /*a*/, double /*b*/, double /*c*/, double /*d*/, double /*e*/, double /*f*/) {}
void bro_CanvasRenderingContext2D_transform(void* /*self*/, double /*a*/, double /*b*/, double /*c*/, double /*d*/, double /*e*/, double /*f*/) {}

void bro_CanvasRenderingContext2D_moveTo(void* /*self*/, double /*x*/, double /*y*/) {}
void bro_CanvasRenderingContext2D_lineTo(void* /*self*/, double /*x*/, double /*y*/) {}
void bro_CanvasRenderingContext2D_arcTo(void* /*self*/, double /*x1*/, double /*y1*/, double /*x2*/, double /*y2*/, double /*radius*/) {}
void bro_CanvasRenderingContext2D_bezierCurveTo(void* /*self*/, double /*cp1x*/, double /*cp1y*/, double /*cp2x*/, double /*cp2y*/, double /*x*/, double /*y*/) {}
void bro_CanvasRenderingContext2D_quadraticCurveTo(void* /*self*/, double /*cpx*/, double /*cpy*/, double /*x*/, double /*y*/) {}
void bro_CanvasRenderingContext2D_arc(void* /*self*/, double /*x*/, double /*y*/, double /*radius*/, double /*startAngle*/, double /*endAngle*/, bool /*counterclockwise*/) {}
void bro_CanvasRenderingContext2D_ellipse(void* /*self*/, double /*x*/, double /*y*/, double /*radiusX*/, double /*radiusY*/, double /*rotation*/, double /*startAngle*/, double /*endAngle*/, bool /*counterclockwise*/) {}
void bro_CanvasRenderingContext2D_rect(void* /*self*/, double /*x*/, double /*y*/, double /*w*/, double /*h*/) {}
bool bro_CanvasRenderingContext2D_isPointInPath(void* /*self*/, double /*x*/, double /*y*/) { return false; }

void bro_CanvasRenderingContext2D_drawImage(void* /*self*/, void* /*image*/, double /*sx*/, double /*sy*/, double /*sw*/, double /*sh*/, double /*dx*/, double /*dy*/, double /*dw*/, double /*dh*/) {}
void* bro_CanvasRenderingContext2D_getImageData(void* /*self*/, double /*sx*/, double /*sy*/, double /*sw*/, double /*sh*/) { return nullptr; }
void bro_CanvasRenderingContext2D_putImageData(void* /*self*/, void* /*imageData*/, double /*dx*/, double /*dy*/, double /*dirtyX*/, double /*dirtyY*/, double /*dirtyWidth*/, double /*dirtyHeight*/) {}
void* bro_CanvasRenderingContext2D_createImageData(void* /*self*/, void* /*swOrImagedata*/, double /*sh*/) { return nullptr; }

void* bro_CanvasRenderingContext2D_createLinearGradient(void* /*self*/, double /*x0*/, double /*y0*/, double /*x1*/, double /*y1*/) {
    return new BroCanvasGradientImpl();
}

void* bro_CanvasRenderingContext2D_createRadialGradient(void* /*self*/, double /*x0*/, double /*y0*/, double /*r0*/, double /*x1*/, double /*y1*/, double /*r1*/) {
    return new BroCanvasGradientImpl();
}

void* bro_CanvasRenderingContext2D_measureText(void* /*self*/, const char* /*text*/) {
    return new BroTextMetricsImpl();
}

void bro_CanvasRenderingContext2D_setLineDash(void* /*self*/, void* /*segments*/) {}
void* bro_CanvasRenderingContext2D_getLineDash(void* /*self*/) { return nullptr; }

} // extern "C"
