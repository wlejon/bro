// =============================================================================
// bro_canvas_c_abi.h — Pure C-ABI declarations for bro.canvas
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, direct native CPU register call.
// =============================================================================

#ifndef BRO_CANVAS_C_ABI_H
#define BRO_CANVAS_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Interface bro.canvas.CanvasGradient ---
void* bro_CanvasGradient_create(void);
void  bro_CanvasGradient_destroy(void* self);
void bro_CanvasGradient_addColorStop(void* self, double offset, const char* color);

// --- Interface bro.canvas.TextMetrics ---
void* bro_TextMetrics_create(void);
void  bro_TextMetrics_destroy(void* self);
double bro_TextMetrics_get_width(void* self);

// --- Interface bro.canvas.CanvasRenderingContext2D ---
void* bro_CanvasRenderingContext2D_create(void);
void  bro_CanvasRenderingContext2D_destroy(void* self);
int32_t bro_CanvasRenderingContext2D_get_canvasWidth(void* self);
int32_t bro_CanvasRenderingContext2D_get_canvasHeight(void* self);
void* bro_CanvasRenderingContext2D_get_fillStyle(void* self);
void bro_CanvasRenderingContext2D_set_fillStyle(void* self, void* val);
void* bro_CanvasRenderingContext2D_get_strokeStyle(void* self);
void bro_CanvasRenderingContext2D_set_strokeStyle(void* self, void* val);
double bro_CanvasRenderingContext2D_get_lineWidth(void* self);
void bro_CanvasRenderingContext2D_set_lineWidth(void* self, double val);
const char* bro_CanvasRenderingContext2D_get_lineCap(void* self);
void bro_CanvasRenderingContext2D_set_lineCap(void* self, const char* val);
const char* bro_CanvasRenderingContext2D_get_lineJoin(void* self);
void bro_CanvasRenderingContext2D_set_lineJoin(void* self, const char* val);
double bro_CanvasRenderingContext2D_get_miterLimit(void* self);
void bro_CanvasRenderingContext2D_set_miterLimit(void* self, double val);
double bro_CanvasRenderingContext2D_get_globalAlpha(void* self);
void bro_CanvasRenderingContext2D_set_globalAlpha(void* self, double val);
const char* bro_CanvasRenderingContext2D_get_globalCompositeOperation(void* self);
void bro_CanvasRenderingContext2D_set_globalCompositeOperation(void* self, const char* val);
double bro_CanvasRenderingContext2D_get_shadowOffsetX(void* self);
void bro_CanvasRenderingContext2D_set_shadowOffsetX(void* self, double val);
double bro_CanvasRenderingContext2D_get_shadowOffsetY(void* self);
void bro_CanvasRenderingContext2D_set_shadowOffsetY(void* self, double val);
double bro_CanvasRenderingContext2D_get_shadowBlur(void* self);
void bro_CanvasRenderingContext2D_set_shadowBlur(void* self, double val);
const char* bro_CanvasRenderingContext2D_get_shadowColor(void* self);
void bro_CanvasRenderingContext2D_set_shadowColor(void* self, const char* val);
const char* bro_CanvasRenderingContext2D_get_font(void* self);
void bro_CanvasRenderingContext2D_set_font(void* self, const char* val);
const char* bro_CanvasRenderingContext2D_get_textAlign(void* self);
void bro_CanvasRenderingContext2D_set_textAlign(void* self, const char* val);
const char* bro_CanvasRenderingContext2D_get_textBaseline(void* self);
void bro_CanvasRenderingContext2D_set_textBaseline(void* self, const char* val);
const char* bro_CanvasRenderingContext2D_get_direction(void* self);
void bro_CanvasRenderingContext2D_set_direction(void* self, const char* val);
bool bro_CanvasRenderingContext2D_get_imageSmoothingEnabled(void* self);
void bro_CanvasRenderingContext2D_set_imageSmoothingEnabled(void* self, bool val);
const char* bro_CanvasRenderingContext2D_get_imageSmoothingQuality(void* self);
void bro_CanvasRenderingContext2D_set_imageSmoothingQuality(void* self, const char* val);
double bro_CanvasRenderingContext2D_get_lineDashOffset(void* self);
void bro_CanvasRenderingContext2D_set_lineDashOffset(void* self, double val);
void bro_CanvasRenderingContext2D_save(void* self);
void bro_CanvasRenderingContext2D_restore(void* self);
void bro_CanvasRenderingContext2D_reset(void* self);
void bro_CanvasRenderingContext2D_beginPath(void* self);
void bro_CanvasRenderingContext2D_closePath(void* self);
void bro_CanvasRenderingContext2D_stroke(void* self);
void bro_CanvasRenderingContext2D_fill(void* self);
void bro_CanvasRenderingContext2D_clip(void* self);
void bro_CanvasRenderingContext2D_resetTransform(void* self);
void bro_CanvasRenderingContext2D_fillRect(void* self, double x, double y, double w, double h);
void bro_CanvasRenderingContext2D_strokeRect(void* self, double x, double y, double w, double h);
void bro_CanvasRenderingContext2D_clearRect(void* self, double x, double y, double w, double h);
void bro_CanvasRenderingContext2D_fillText(void* self, const char* text, double x, double y, double maxWidth);
void bro_CanvasRenderingContext2D_strokeText(void* self, const char* text, double x, double y, double maxWidth);
void bro_CanvasRenderingContext2D_translate(void* self, double x, double y);
void bro_CanvasRenderingContext2D_rotate(void* self, double angle);
void bro_CanvasRenderingContext2D_scale(void* self, double x, double y);
void bro_CanvasRenderingContext2D_setTransform(void* self, double a, double b, double c, double d, double e, double f);
void bro_CanvasRenderingContext2D_transform(void* self, double a, double b, double c, double d, double e, double f);
void bro_CanvasRenderingContext2D_moveTo(void* self, double x, double y);
void bro_CanvasRenderingContext2D_lineTo(void* self, double x, double y);
void bro_CanvasRenderingContext2D_arcTo(void* self, double x1, double y1, double x2, double y2, double radius);
void bro_CanvasRenderingContext2D_bezierCurveTo(void* self, double cp1x, double cp1y, double cp2x, double cp2y, double x, double y);
void bro_CanvasRenderingContext2D_quadraticCurveTo(void* self, double cpx, double cpy, double x, double y);
void bro_CanvasRenderingContext2D_arc(void* self, double x, double y, double radius, double startAngle, double endAngle, bool counterclockwise);
void bro_CanvasRenderingContext2D_ellipse(void* self, double x, double y, double radiusX, double radiusY, double rotation, double startAngle, double endAngle, bool counterclockwise);
void bro_CanvasRenderingContext2D_rect(void* self, double x, double y, double w, double h);
bool bro_CanvasRenderingContext2D_isPointInPath(void* self, double x, double y);
void bro_CanvasRenderingContext2D_drawImage(void* self, void* image, double sx, double sy, double sw, double sh, double dx, double dy, double dw, double dh);
void* bro_CanvasRenderingContext2D_getImageData(void* self, double sx, double sy, double sw, double sh);
void bro_CanvasRenderingContext2D_putImageData(void* self, void* imageData, double dx, double dy, double dirtyX, double dirtyY, double dirtyWidth, double dirtyHeight);
void* bro_CanvasRenderingContext2D_createImageData(void* self, void* swOrImagedata, double sh);
void* bro_CanvasRenderingContext2D_createLinearGradient(void* self, double x0, double y0, double x1, double y1);
void* bro_CanvasRenderingContext2D_createRadialGradient(void* self, double x0, double y0, double r0, double x1, double y1, double r1);
void* bro_CanvasRenderingContext2D_measureText(void* self, const char* text);
void bro_CanvasRenderingContext2D_setLineDash(void* self, void* segments);
void* bro_CanvasRenderingContext2D_getLineDash(void* self);

#ifdef __cplusplus
}
#endif

#endif // BRO_CANVAS_C_ABI_H
