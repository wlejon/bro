#include "layout/draw_traversal.h"
#include "layout/el_input.h"
#include "layout/el_textarea.h"
#include "layout/el_select.h"
#include "layout/el_svg.h"
#include "canvas/canvas_scene.h"
#include "webgl/webgl2_context.h"
#include "css/transform.h"
#include "dom/element.h"
#include "dom/text_node.h"
#include "dom/node.h"
#include "util/log.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stb_image.h>

#include <include/core/SkColorFilter.h>
#include <include/effects/SkImageFilters.h>

namespace bro::layout {

// CSS transform and transform-origin parsing live in htmlayout
// (htmlayout::css::parseTransform, parseTransformOrigin, Matrix2D).

// ---------------------------------------------------------------------------
// CSS filter parsing → Skia SkImageFilter chain
// Supports: blur, brightness, contrast, grayscale, sepia, saturate,
//           hue-rotate, invert, opacity, drop-shadow
// ---------------------------------------------------------------------------
static sk_sp<SkImageFilter> parseCSSFilter(const std::string& val) {
    sk_sp<SkImageFilter> result;
    size_t pos = 0;
    while (pos < val.size()) {
        while (pos < val.size() && (val[pos] == ' ' || val[pos] == '\t'))
            ++pos;
        if (pos >= val.size()) break;

        size_t nameStart = pos;
        while (pos < val.size() && val[pos] != '(') ++pos;
        std::string func = val.substr(nameStart, pos - nameStart);
        // Trim trailing whitespace from function name
        while (!func.empty() && func.back() == ' ') func.pop_back();
        if (pos >= val.size()) break;
        ++pos; // skip '('

        // Parse the argument(s)
        auto readFloat = [&]() -> float {
            while (pos < val.size() && (val[pos] == ' ' || val[pos] == ','))
                ++pos;
            char* end = nullptr;
            float v = std::strtof(val.c_str() + pos, &end);
            pos = static_cast<size_t>(end - val.c_str());
            // Handle % suffix
            if (pos < val.size() && val[pos] == '%') {
                v /= 100.0f;
                ++pos;
            }
            // Skip unit suffixes (px, deg, rad, turn)
            while (pos < val.size() && std::isalpha(static_cast<unsigned char>(val[pos])))
                ++pos;
            return v;
        };

        if (func == "blur") {
            float sigma = readFloat();
            result = SkImageFilters::Blur(sigma, sigma, std::move(result));
        } else if (func == "brightness") {
            float v = readFloat();
            // brightness: multiply RGB by v
            float m[20] = {
                v, 0, 0, 0, 0,
                0, v, 0, 0, 0,
                0, 0, v, 0, 0,
                0, 0, 0, 1, 0
            };
            auto cf = SkColorFilters::Matrix(m);
            result = SkImageFilters::ColorFilter(std::move(cf), std::move(result));
        } else if (func == "contrast") {
            float v = readFloat();
            // contrast: scale RGB around 0.5
            float t = 0.5f * (1.0f - v);
            float m[20] = {
                v, 0, 0, 0, t,
                0, v, 0, 0, t,
                0, 0, v, 0, t,
                0, 0, 0, 1, 0
            };
            auto cf = SkColorFilters::Matrix(m);
            result = SkImageFilters::ColorFilter(std::move(cf), std::move(result));
        } else if (func == "grayscale") {
            float v = readFloat();
            v = std::clamp(v, 0.0f, 1.0f);
            float inv = 1.0f - v;
            // ITU-R BT.601 luma coefficients
            float m[20] = {
                0.2126f + 0.7874f * inv, 0.7152f - 0.7152f * inv, 0.0722f - 0.0722f * inv, 0, 0,
                0.2126f - 0.2126f * inv, 0.7152f + 0.2848f * inv, 0.0722f - 0.0722f * inv, 0, 0,
                0.2126f - 0.2126f * inv, 0.7152f - 0.7152f * inv, 0.0722f + 0.9278f * inv, 0, 0,
                0, 0, 0, 1, 0
            };
            auto cf = SkColorFilters::Matrix(m);
            result = SkImageFilters::ColorFilter(std::move(cf), std::move(result));
        } else if (func == "sepia") {
            float v = readFloat();
            v = std::clamp(v, 0.0f, 1.0f);
            float inv = 1.0f - v;
            float m[20] = {
                0.393f + 0.607f * inv, 0.769f - 0.769f * inv, 0.189f - 0.189f * inv, 0, 0,
                0.349f - 0.349f * inv, 0.686f + 0.314f * inv, 0.168f - 0.168f * inv, 0, 0,
                0.272f - 0.272f * inv, 0.534f - 0.534f * inv, 0.131f + 0.869f * inv, 0, 0,
                0, 0, 0, 1, 0
            };
            auto cf = SkColorFilters::Matrix(m);
            result = SkImageFilters::ColorFilter(std::move(cf), std::move(result));
        } else if (func == "saturate") {
            float v = readFloat();
            // Same as grayscale but inverted: v=1 is identity, v=0 is full grayscale
            float m[20] = {
                0.2126f + 0.7874f * v, 0.7152f - 0.7152f * v, 0.0722f - 0.0722f * v, 0, 0,
                0.2126f - 0.2126f * v, 0.7152f + 0.2848f * v, 0.0722f - 0.0722f * v, 0, 0,
                0.2126f - 0.2126f * v, 0.7152f - 0.7152f * v, 0.0722f + 0.9278f * v, 0, 0,
                0, 0, 0, 1, 0
            };
            auto cf = SkColorFilters::Matrix(m);
            result = SkImageFilters::ColorFilter(std::move(cf), std::move(result));
        } else if (func == "hue-rotate") {
            // Read angle in degrees
            while (pos < val.size() && (val[pos] == ' ')) ++pos;
            char* end = nullptr;
            float deg = std::strtof(val.c_str() + pos, &end);
            pos = static_cast<size_t>(end - val.c_str());
            // Skip unit
            while (pos < val.size() && std::isalpha(static_cast<unsigned char>(val[pos])))
                ++pos;
            float rad = deg * 3.14159265f / 180.0f;
            float cosA = std::cos(rad), sinA = std::sin(rad);
            // Hue rotation matrix (rotate in RGB color space around the gray axis)
            float m[20] = {
                0.213f + cosA * 0.787f - sinA * 0.213f,
                0.715f - cosA * 0.715f - sinA * 0.715f,
                0.072f - cosA * 0.072f + sinA * 0.928f, 0, 0,
                0.213f - cosA * 0.213f + sinA * 0.143f,
                0.715f + cosA * 0.285f + sinA * 0.140f,
                0.072f - cosA * 0.072f - sinA * 0.283f, 0, 0,
                0.213f - cosA * 0.213f - sinA * 0.787f,
                0.715f - cosA * 0.715f + sinA * 0.715f,
                0.072f + cosA * 0.928f + sinA * 0.072f, 0, 0,
                0, 0, 0, 1, 0
            };
            auto cf = SkColorFilters::Matrix(m);
            result = SkImageFilters::ColorFilter(std::move(cf), std::move(result));
        } else if (func == "invert") {
            float v = readFloat();
            v = std::clamp(v, 0.0f, 1.0f);
            // invert: lerp between identity and full inversion
            float s = 1.0f - 2.0f * v; // scale: 1 at v=0, -1 at v=1
            float t = v;               // translate: 0 at v=0, 1 at v=1
            float m[20] = {
                s, 0, 0, 0, t,
                0, s, 0, 0, t,
                0, 0, s, 0, t,
                0, 0, 0, 1, 0
            };
            auto cf = SkColorFilters::Matrix(m);
            result = SkImageFilters::ColorFilter(std::move(cf), std::move(result));
        } else if (func == "opacity") {
            float v = readFloat();
            float m[20] = {
                1, 0, 0, 0, 0,
                0, 1, 0, 0, 0,
                0, 0, 1, 0, 0,
                0, 0, 0, v, 0
            };
            auto cf = SkColorFilters::Matrix(m);
            result = SkImageFilters::ColorFilter(std::move(cf), std::move(result));
        } else if (func == "drop-shadow") {
            float dx = readFloat();
            float dy = readFloat();
            float blur = readFloat();
            // Try to parse color (remaining tokens before ')')
            render::Color sc = {0, 0, 0, 255};
            size_t colorStart = pos;
            while (pos < val.size() && val[pos] != ')') ++pos;
            std::string colorStr = val.substr(colorStart, pos - colorStart);
            // Trim
            size_t ca = colorStr.find_first_not_of(" \t");
            if (ca != std::string::npos) {
                colorStr = colorStr.substr(ca);
                size_t cb = colorStr.find_last_not_of(" \t");
                if (cb != std::string::npos) colorStr = colorStr.substr(0, cb + 1);
                DrawTraversal::tryParseColor(colorStr, sc);
            }
            SkColor skc = SkColorSetARGB(sc.a, sc.r, sc.g, sc.b);
            auto shadow = SkImageFilters::DropShadow(dx, dy, blur / 2.0f, blur / 2.0f, skc, std::move(result));
            result = std::move(shadow);
        }

        // Skip to closing paren
        while (pos < val.size() && val[pos] != ')') ++pos;
        if (pos < val.size()) ++pos;
    }
    return result;
}

/// Get the effective vertical overflow value, checking overflow-y then overflow.
static std::string getOverflowY(const htmlayout::css::ComputedStyle& style) {
    auto oyIt = style.find("overflow-y");
    if (oyIt != style.end()) return oyIt->second;
    auto oIt = style.find("overflow");
    if (oIt != style.end()) return oIt->second;
    return "visible";
}

DrawTraversal::DrawTraversal(render::Renderer* renderer, FontManager* fontManager)
    : renderer_(renderer), fontManager_(fontManager) {}

void DrawTraversal::draw(dom::Element* root, float scrollX, float scrollY,
                         int viewportW, int viewportH, int viewportTop) {
    if (!root || !renderer_) return;
    viewportW_ = viewportW;
    viewportH_ = viewportH;
    viewportTop_ = viewportTop;
    drawElement(root, scrollX, scrollY);
}

void DrawTraversal::drawElement(dom::Element* elem, float offsetX, float offsetY) {
    if (!elem) return;
    drawElementContent(elem, offsetX, offsetY);
}

void DrawTraversal::drawNode(dom::Node* node, float offsetX, float offsetY) {
    if (!node) return;

    if (node->nodeType() == dom::NodeType::Element) {
        drawElementContent(static_cast<dom::Element*>(node), offsetX, offsetY);
    } else if (node->nodeType() == dom::NodeType::Text) {
        auto* parent = node->parentNode();
        if (parent && parent->nodeType() == dom::NodeType::Element) {
            drawText(node, static_cast<dom::Element*>(parent), offsetX, offsetY);
        }
    }
}

// Parse a CSS length value (px, em, %) into pixels. Percentage is relative to ref.
static float parseLengthPx(const std::string& val, float ref = 0) {
    if (val.empty()) return 0;
    char* end = nullptr;
    float v = std::strtof(val.c_str(), &end);
    if (end == val.c_str()) return 0;
    if (end && *end == '%') return v * ref / 100.0f;
    return v; // px or unitless
}

// Get the border-radius for an element. Returns the average of all four corners
// (simplified — full per-corner elliptical radii would need renderer changes).
static float getBorderRadius(const htmlayout::css::ComputedStyle& style) {
    auto brIt = style.find("border-radius");
    if (brIt != style.end() && !brIt->second.empty()) {
        float v = parseLengthPx(brIt->second);
        if (v > 0) return v;
    }
    float sum = 0; int count = 0;
    for (const char* prop : {"border-top-left-radius", "border-top-right-radius",
                             "border-bottom-left-radius", "border-bottom-right-radius"}) {
        auto it = style.find(prop);
        if (it != style.end()) {
            float v = parseLengthPx(it->second);
            if (v > 0) { sum += v; ++count; }
        }
    }
    return count > 0 ? sum / count : 0;
}

void DrawTraversal::drawElementContent(dom::Element* elem, float offsetX, float offsetY) {
    if (!elem) return;

    auto& style = elem->computedStyle();

    // Check display:none
    auto dispIt = style.find("display");
    if (dispIt != style.end() && dispIt->second == "none") return;

    // Check visibility:hidden (still occupies space but not drawn)
    bool visible = true;
    auto visIt = style.find("visibility");
    if (visIt != style.end() && visIt->second == "hidden") visible = false;

    auto& box = elem->layoutBox();
    float x = box.contentRect.x + offsetX;
    float y = box.contentRect.y + offsetY;
    float w = box.contentRect.width;
    float h = box.contentRect.height;

    // Border box for background/border drawing
    float bx = x - box.padding.left - box.border.left;
    float by = y - box.padding.top - box.border.top;
    float bw = box.fullWidth();
    float bh = box.fullHeight();

    // CSS Transform: wrap entire element drawing in a transform
    bool hasTransform = false;
    {
        auto trIt = style.find("transform");
        if (trIt != style.end() && !trIt->second.empty() && trIt->second != "none") {
            auto mat = htmlayout::css::parseTransform(trIt->second, bw, bh);
            if (!mat.isIdentity()) {
                hasTransform = true;
                float ox, oy;
                auto toIt = style.find("transform-origin");
                std::string_view originVal =
                    (toIt != style.end()) ? std::string_view(toIt->second)
                                          : std::string_view();
                htmlayout::css::parseTransformOrigin(originVal, bw, bh, ox, oy);
                // Apply: translate to origin, concat matrix, translate back
                renderer_->save();
                renderer_->translate(bx + ox, by + oy);
                renderer_->concat(mat.a, mat.b, mat.c, mat.d, mat.e, mat.f);
                renderer_->translate(-(bx + ox), -(by + oy));
            }
        }
    }

    // Opacity: wrap entire element in a layer
    bool hasOpacity = false;
    auto opIt = style.find("opacity");
    if (opIt != style.end()) {
        float opacity = std::clamp(std::strtof(opIt->second.c_str(), nullptr), 0.0f, 1.0f);
        if (opacity < 1.0f) {
            hasOpacity = true;
            renderer_->saveLayerAlpha(static_cast<uint8_t>(opacity * 255));
        }
    }

    // CSS filter: wrap element drawing in a filter layer
    bool hasFilter = false;
    {
        auto fIt = style.find("filter");
        if (fIt != style.end() && !fIt->second.empty() && fIt->second != "none") {
            auto filter = parseCSSFilter(fIt->second);
            if (filter) {
                hasFilter = true;
                // Use a generous bounds that includes blur/shadow overflow
                renderer_->saveLayerWithFilter(filter.get(),
                    bx - 50, by - 50, bw + 100, bh + 100);
            }
        }
    }

    if (visible) {
        // Box shadows (drawn before background, behind the element).
        // Supports multiple comma-separated shadows.  CSS spec: first shadow
        // in the list is drawn on top (closest to element), so we draw in
        // reverse order.
        auto bsIt = style.find("box-shadow");
        if (bsIt != style.end() && !bsIt->second.empty() && bsIt->second != "none") {
            float radius = getBorderRadius(style);

            // Split on commas, respecting parentheses (for rgb()/rgba())
            std::vector<std::string> shadows;
            {
                const auto& full = bsIt->second;
                int depth = 0;
                size_t start = 0;
                for (size_t i = 0; i <= full.size(); ++i) {
                    if (i < full.size() && full[i] == '(') ++depth;
                    else if (i < full.size() && full[i] == ')') --depth;
                    else if ((i == full.size() || full[i] == ',') && depth <= 0) {
                        std::string s = full.substr(start, i - start);
                        // Trim leading/trailing whitespace
                        size_t a = s.find_first_not_of(" \t");
                        size_t b = s.find_last_not_of(" \t");
                        if (a != std::string::npos)
                            shadows.push_back(s.substr(a, b - a + 1));
                        start = i + 1;
                    }
                }
            }

            // Draw in reverse order (last shadow = furthest from element = drawn first)
            for (int si = static_cast<int>(shadows.size()) - 1; si >= 0; --si) {
                std::string val = shadows[si];
                bool inset = false;
                auto ipos = val.find("inset");
                if (ipos != std::string::npos) {
                    inset = true;
                    val.erase(ipos, 5);
                }
                std::istringstream iss(val);
                std::vector<float> nums;
                std::string colorStr;
                std::string token;
                while (iss >> token) {
                    char* end = nullptr;
                    float v = std::strtof(token.c_str(), &end);
                    if (end != token.c_str() && (*end == '\0' || *end == 'p'))
                        nums.push_back(v);
                    else {
                        if (!colorStr.empty()) colorStr += ' ';
                        colorStr += token;
                    }
                }
                if (nums.size() >= 2) {
                    float sdx = nums[0], sdy = nums[1];
                    float sblur = nums.size() >= 3 ? nums[2] : 0;
                    float sspread = nums.size() >= 4 ? nums[3] : 0;
                    render::Color sc = {0, 0, 0, 80};
                    if (!colorStr.empty()) tryParseColor(colorStr, sc);
                    renderer_->drawBoxShadow(bx, by, bw, bh, radius, radius,
                                            sdx, sdy, sblur, sspread, sc, inset);
                }
            }
        }

        // For html/body elements, background covers the entire viewport (CSS2.1 spec).
        // viewportTop_ offsets for engine-reserved insets (e.g. menu bar).
        std::string tag = elem->tagName();
        if ((tag == "html" || tag == "HTML" || tag == "body" || tag == "BODY") &&
            viewportW_ > 0 && viewportH_ > 0) {
            drawBackground(elem, 0, static_cast<float>(viewportTop_),
                           static_cast<float>(viewportW_), static_cast<float>(viewportH_));
        } else {
            drawBackground(elem, bx, by, bw, bh);
        }

        // Draw borders
        drawBorders(elem, bx, by, bw, bh);

        // Draw outline (outside the border box)
        auto olwIt = style.find("outline-width");
        auto olsIt = style.find("outline-style");
        if (olwIt != style.end() && olsIt != style.end() && olsIt->second != "none") {
            float olw = parseLengthPx(olwIt->second);
            if (olw > 0) {
                render::Color olc = {0, 0, 0, 255};
                auto olcIt = style.find("outline-color");
                if (olcIt != style.end()) tryParseColor(olcIt->second, olc);
                float olOff = 0;
                auto oloIt = style.find("outline-offset");
                if (oloIt != style.end()) olOff = parseLengthPx(oloIt->second);
                float ox = bx - olw - olOff;
                float oy = by - olw - olOff;
                float ow = bw + 2 * (olw + olOff);
                float oh = bh + 2 * (olw + olOff);
                // Top, Bottom, Left, Right as filled rects
                renderer_->fillRect(ox, oy, ow, olw, olc);
                renderer_->fillRect(ox, oy + oh - olw, ow, olw, olc);
                renderer_->fillRect(ox, oy + olw, olw, oh - 2*olw, olc);
                renderer_->fillRect(ox + ow - olw, oy + olw, olw, oh - 2*olw, olc);
            }
        }

        // Draw list marker (bullet/number for <li> elements)
        std::string tagLower = elem->tagName();
        for (auto& c : tagLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (tagLower == "li") {
            auto lstIt = style.find("list-style-type");
            std::string listType = (lstIt != style.end()) ? lstIt->second : "disc";
            if (listType != "none") {
                float markerX = bx - 20.0f;
                float markerY = by + bh / 2.0f;
                render::Color mc = {0, 0, 0, 255};
                auto mcIt = style.find("color");
                if (mcIt != style.end()) tryParseColor(mcIt->second, mc);

                if (listType == "disc") {
                    renderer_->drawCircle(markerX, markerY, 3.0f, mc, mc, 0);
                } else if (listType == "circle") {
                    render::Color none = {0,0,0,0};
                    renderer_->drawCircle(markerX, markerY, 3.0f, none, mc, 1.0f);
                } else if (listType == "square") {
                    renderer_->fillRect(markerX - 3, markerY - 3, 6, 6, mc);
                } else if (listType == "decimal" || listType == "decimal-leading-zero") {
                    // Count position among siblings
                    int idx = 1;
                    auto* parent = elem->parentNode();
                    if (parent) {
                        for (auto* sib : parent->childNodes()) {
                            if (sib == elem) break;
                            if (sib->nodeType() == dom::NodeType::Element) ++idx;
                        }
                    }
                    std::string num = std::to_string(idx) + ".";
                    uint64_t font = getFontHandle(elem);
                    if (font) {
                        auto tm = renderer_->measureText(num, font);
                        renderer_->drawText(num, markerX - tm.width, markerY + tm.ascent / 2, font, mc);
                    }
                }
            }
        }
    }

    // Check for overflow clipping
    bool needsClip = false;
    std::string overflow = getOverflowY(style);
    if (overflow == "hidden" || overflow == "scroll" || overflow == "auto") {
        needsClip = true;
        renderer_->save();
        renderer_->setClip(bx, by, bw, bh);
    }

    // SVG elements render their own children via the SVG pipeline — skip DOM traversal
    if (elem->svgControl()) {
        // Draw the SVG control, then return (no child traversal)
        if (visible) {
            elem->svgControl()->draw(renderer_, elem, box, offsetX, offsetY);
        }
        if (needsClip) {
            renderer_->restore();
        }
        if (hasFilter) renderer_->restore();
        if (hasOpacity) renderer_->restore();
        if (hasTransform) renderer_->restore();
        return;
    }

    // Canvas/WebGL/SceneGraph elements: trigger a layer break so the compositor
    // can interleave canvas textures with HTML layers in document order.
    // SceneGraph check must come first since scene elements also have a canvasScene.
    if (elem->sceneGraph() && visible) {
        // 3D mesh FBO layer (texture ID stored on element by scene graph render)
        unsigned int fboTex = elem->sceneGraphFBOTexture();
        if (fboTex && layerBreakCb_) {
            layerBreakCb_(nullptr, fboTex, x, y, w, h);
        }
        // 2D canvas layer (for ShapeNode/SpriteNode content)
        if (elem->canvasScene() && layerBreakCb_) {
            auto* scene = static_cast<canvas::CanvasScene*>(elem->canvasScene());
            layerBreakCb_(scene, 0, x, y, w, h);
        }
        if (needsClip) renderer_->restore();
        if (hasFilter) renderer_->restore();
        if (hasOpacity) renderer_->restore();
        if (hasTransform) renderer_->restore();
        return;
    }
    if (elem->canvasScene() && visible) {
        auto* scene = static_cast<canvas::CanvasScene*>(elem->canvasScene());
        if (layerBreakCb_) {
            layerBreakCb_(scene, 0, x, y, w, h);
        }
        if (needsClip) renderer_->restore();
        if (hasFilter) renderer_->restore();
        if (hasOpacity) renderer_->restore();
        if (hasTransform) renderer_->restore();
        return;
    }
    if (elem->webglContext() && visible) {
        auto* webglCtx = static_cast<webgl::WebGL2RenderingContext*>(elem->webglContext());
        if (layerBreakCb_) {
            layerBreakCb_(nullptr, webglCtx->colorTexture(), x, y, w, h);
        }
        if (needsClip) renderer_->restore();
        if (hasFilter) renderer_->restore();
        if (hasOpacity) renderer_->restore();
        if (hasTransform) renderer_->restore();
        return;
    }

    // Children's offset is the parent's absolute content position
    // (so child positions, which are relative to parent content area, become absolute)
    float childOffsetX = x;
    // Clamp scrollTop to valid range — JS may have set it before layout updated
    float maxST = std::max(0.0f, box.naturalHeight - box.contentRect.height);
    float scrollTop = std::clamp(elem->scrollTopValue(), 0.0f, maxST);
    float childOffsetY = y - scrollTop;

    // Draw composed children (shadow DOM + slot replacement)
    for (auto* child : elem->composedChildNodes())
        drawNode(child, childOffsetX, childOffsetY);

    // Draw replaced element content (input, textarea, select, svg)
    if (visible) {
        auto* inputCtrl = elem->inputControl();
        if (inputCtrl) {
            inputCtrl->draw(renderer_, box, style, offsetX, offsetY);
        }
        auto* textareaCtrl = elem->textareaControl();
        if (textareaCtrl) {
            textareaCtrl->draw(renderer_, box, style, offsetX, offsetY);
        }
        auto* selectCtrl = elem->selectControl();
        if (selectCtrl) {
            selectCtrl->draw(renderer_, box, style, offsetX, offsetY);
        }
    }

    if (needsClip) {
        renderer_->restore();
    }
    if (hasFilter) renderer_->restore();
    if (hasOpacity) renderer_->restore();
    if (hasTransform) renderer_->restore();
}

void DrawTraversal::drawBackground(dom::Element* elem, float x, float y, float w, float h) {
    auto& style = elem->computedStyle();
    float radius = getBorderRadius(style);

    // Background color
    auto bgIt = style.find("background-color");
    if (bgIt != style.end() && !bgIt->second.empty()) {
        render::Color c;
        if (tryParseColor(bgIt->second, c) && c.a > 0) {
            if (radius > 0)
                renderer_->fillRoundRect(x, y, w, h, radius, radius, c);
            else
                renderer_->fillRect(x, y, w, h, c);
        }
    }

    // Background image
    auto imgIt = style.find("background-image");
    if (imgIt != style.end() && !imgIt->second.empty() && imgIt->second != "none") {
        // Handle url(...) and gradients
        const std::string& val = imgIt->second;
        if (val.substr(0, 4) == "url(") {
            // Extract URL
            size_t start = val.find('(') + 1;
            size_t end = val.rfind(')');
            if (end > start) {
                std::string url = val.substr(start, end - start);
                if (!url.empty() && (url.front() == '"' || url.front() == '\'')) {
                    url = url.substr(1, url.size() - 2);
                }
                loadImage(url, basePath_);
                auto imgCacheIt = imageCache_.find(url);
                if (imgCacheIt != imageCache_.end() && !imgCacheIt->second.data.empty()) {
                    float imgW = static_cast<float>(imgCacheIt->second.width);
                    float imgH = static_cast<float>(imgCacheIt->second.height);
                    float drawW = imgW > 0 ? imgW : w;
                    float drawH = imgH > 0 ? imgH : h;

                    // background-size
                    auto bsIt2 = style.find("background-size");
                    if (bsIt2 != style.end()) {
                        const auto& bs = bsIt2->second;
                        if (bs == "cover" && imgW > 0 && imgH > 0) {
                            float scale = std::max(w / imgW, h / imgH);
                            drawW = imgW * scale; drawH = imgH * scale;
                        } else if (bs == "contain" && imgW > 0 && imgH > 0) {
                            float scale = std::min(w / imgW, h / imgH);
                            drawW = imgW * scale; drawH = imgH * scale;
                        } else if (bs != "auto") {
                            // Parse "Wpx Hpx" or "W% H%"
                            std::istringstream bsIss(bs);
                            std::string wStr, hStr;
                            bsIss >> wStr;
                            bsIss >> hStr;
                            if (!wStr.empty() && wStr != "auto")
                                drawW = parseLengthPx(wStr, w);
                            if (!hStr.empty() && hStr != "auto")
                                drawH = parseLengthPx(hStr, h);
                            else if (imgW > 0 && imgH > 0 && !wStr.empty() && wStr != "auto")
                                drawH = drawW * imgH / imgW; // maintain aspect ratio
                        }
                    }

                    // background-position
                    float posX = x, posY = y;
                    auto bpIt = style.find("background-position");
                    if (bpIt != style.end() && !bpIt->second.empty()) {
                        const auto& bp = bpIt->second;
                        if (bp == "center") {
                            posX = x + (w - drawW) / 2;
                            posY = y + (h - drawH) / 2;
                        } else if (bp == "right") {
                            posX = x + w - drawW;
                        } else if (bp == "bottom") {
                            posY = y + h - drawH;
                        } else {
                            std::istringstream bpIss(bp);
                            std::string pxStr, pyStr;
                            bpIss >> pxStr;
                            bpIss >> pyStr;
                            if (!pxStr.empty()) posX = x + parseLengthPx(pxStr, w);
                            if (!pyStr.empty()) posY = y + parseLengthPx(pyStr, h);
                        }
                    }

                    // background-repeat
                    auto brIt2 = style.find("background-repeat");
                    std::string repeat = (brIt2 != style.end()) ? brIt2->second : "repeat";

                    if (repeat == "no-repeat") {
                        renderer_->drawImage(imgCacheIt->second.data.data(),
                                            imgCacheIt->second.data.size(),
                                            posX, posY, drawW, drawH);
                    } else {
                        // Tile the image
                        renderer_->save();
                        renderer_->setClip(x, y, w, h);
                        bool repeatX = (repeat == "repeat" || repeat == "repeat-x");
                        bool repeatY = (repeat == "repeat" || repeat == "repeat-y");
                        float startX = repeatX ? x - std::fmod(posX - x, drawW) - drawW : posX;
                        float startY = repeatY ? y - std::fmod(posY - y, drawH) - drawH : posY;
                        float endX = repeatX ? x + w : startX + drawW;
                        float endY = repeatY ? y + h : startY + drawH;
                        for (float iy = startY; iy < endY; iy += drawH) {
                            for (float ix = startX; ix < endX; ix += drawW) {
                                renderer_->drawImage(imgCacheIt->second.data.data(),
                                                    imgCacheIt->second.data.size(),
                                                    ix, iy, drawW, drawH);
                            }
                        }
                        renderer_->restore();
                    }
                }
            }
        }
        else if (val.find("linear-gradient") != std::string::npos ||
                 val.find("radial-gradient") != std::string::npos ||
                 val.find("conic-gradient") != std::string::npos) {
            // Parse gradient color stops from the CSS value
            auto parenStart = val.find('(');
            auto parenEnd = val.rfind(')');
            if (parenStart != std::string::npos && parenEnd != std::string::npos) {
                std::string inner = val.substr(parenStart + 1, parenEnd - parenStart - 1);
                // Split on commas (respecting nested parens)
                std::vector<std::string> parts;
                int depth = 0;
                std::string cur;
                for (char c : inner) {
                    if (c == '(') ++depth;
                    else if (c == ')') --depth;
                    else if (c == ',' && depth == 0) {
                        parts.push_back(cur);
                        cur.clear();
                        continue;
                    }
                    cur += c;
                }
                if (!cur.empty()) parts.push_back(cur);

                // Parse direction/angle for linear-gradient
                float angleDeg = 180; // default: to bottom
                size_t colorStart = 0;
                if (val.find("linear-gradient") != std::string::npos && !parts.empty()) {
                    std::string first = parts[0];
                    // Trim
                    while (!first.empty() && first.front() == ' ') first.erase(first.begin());
                    while (!first.empty() && first.back() == ' ') first.pop_back();
                    if (first.find("to ") == 0) {
                        if (first == "to right") angleDeg = 90;
                        else if (first == "to left") angleDeg = 270;
                        else if (first == "to top") angleDeg = 0;
                        else if (first == "to bottom") angleDeg = 180;
                        else if (first == "to top right" || first == "to right top") angleDeg = 45;
                        else if (first == "to bottom right" || first == "to right bottom") angleDeg = 135;
                        else if (first == "to bottom left" || first == "to left bottom") angleDeg = 225;
                        else if (first == "to top left" || first == "to left top") angleDeg = 315;
                        colorStart = 1;
                    } else {
                        char* end = nullptr;
                        float a = std::strtof(first.c_str(), &end);
                        if (end != first.c_str()) {
                            angleDeg = a;
                            colorStart = 1;
                        }
                    }
                }

                // Parse color stops
                std::vector<render::ColorStop> stops;
                size_t numColors = parts.size() - colorStart;
                for (size_t i = colorStart; i < parts.size(); ++i) {
                    std::string part = parts[i];
                    while (!part.empty() && part.front() == ' ') part.erase(part.begin());
                    while (!part.empty() && part.back() == ' ') part.pop_back();

                    render::Color sc = {0,0,0,255};
                    float offset = -1;
                    // Try to extract a percentage at the end
                    auto pctPos = part.rfind('%');
                    if (pctPos != std::string::npos) {
                        // Find the number before %
                        size_t numStart = pctPos;
                        while (numStart > 0 && (std::isdigit(static_cast<unsigned char>(part[numStart-1])) || part[numStart-1] == '.' || part[numStart-1] == ' ')) --numStart;
                        std::string pctStr = part.substr(numStart, pctPos - numStart);
                        while (!pctStr.empty() && pctStr.front() == ' ') pctStr.erase(pctStr.begin());
                        offset = std::strtof(pctStr.c_str(), nullptr) / 100.0f;
                        part = part.substr(0, numStart);
                        while (!part.empty() && part.back() == ' ') part.pop_back();
                    }
                    tryParseColor(part, sc);
                    if (offset < 0) {
                        offset = numColors > 1 ? static_cast<float>(i - colorStart) / static_cast<float>(numColors - 1) : 0;
                    }
                    stops.push_back({offset, sc});
                }

                if (stops.size() >= 2) {
                    if (val.find("linear-gradient") != std::string::npos) {
                        float rad = angleDeg * 3.14159265f / 180.0f;
                        float cx2 = w / 2, cy2 = h / 2;
                        float dx = std::sin(rad) * cx2;
                        float dy = -std::cos(rad) * cy2;
                        renderer_->fillLinearGradient(x, y, w, h,
                            cx2 - dx, cy2 - dy, cx2 + dx, cy2 + dy, stops);
                    } else if (val.find("radial-gradient") != std::string::npos) {
                        renderer_->fillRadialGradient(x, y, w, h,
                            w/2, h/2, w/2, h/2, stops);
                    } else if (val.find("conic-gradient") != std::string::npos) {
                        renderer_->fillConicGradient(x, y, w, h,
                            w/2, h/2, 0, stops);
                    }
                }
            }
        }
    }
}

void DrawTraversal::drawBorders(dom::Element* elem, float x, float y, float w, float h) {
    auto& box = elem->layoutBox();
    auto& style = elem->computedStyle();
    float radius = getBorderRadius(style);

    auto getBorderColor = [&](const char* prop) -> render::Color {
        render::Color c = {0, 0, 0, 255};
        auto it = style.find(prop);
        if (it != style.end()) tryParseColor(it->second, c);
        return c;
    };
    auto isBorderVisible = [&](const char* styleProp) -> bool {
        auto it = style.find(styleProp);
        return it == style.end() || it->second != "none";
    };

    // When all borders have the same color and width, draw as a single rounded/rect stroke
    bool allSameColor = true;
    bool anyVisible = false;
    render::Color firstColor = {0, 0, 0, 255};
    float sides[] = {box.border.top, box.border.right, box.border.bottom, box.border.left};
    const char* colorProps[] = {"border-top-color", "border-right-color",
                                "border-bottom-color", "border-left-color"};
    const char* styleProps[] = {"border-top-style", "border-right-style",
                                "border-bottom-style", "border-left-style"};
    bool firstSet = false;
    for (int i = 0; i < 4; ++i) {
        if (sides[i] > 0 && isBorderVisible(styleProps[i])) {
            auto c = getBorderColor(colorProps[i]);
            if (!firstSet) { firstColor = c; firstSet = true; }
            else if (c.r != firstColor.r || c.g != firstColor.g ||
                     c.b != firstColor.b || c.a != firstColor.a) {
                allSameColor = false;
            }
            anyVisible = true;
        }
    }

    if (!anyVisible) return;

    if (radius > 0 && allSameColor) {
        // Draw a single rounded rect outline
        float avgWidth = 0; int count = 0;
        for (float s : sides) { if (s > 0) { avgWidth += s; ++count; } }
        if (count > 0) avgWidth /= count;
        // Inset by half the border width so the stroke aligns with the border box edge
        float half = avgWidth / 2;
        renderer_->drawRoundRect(x + half, y + half, w - avgWidth, h - avgWidth,
                                 radius, radius, firstColor);
        return;
    }

    // Fallback: draw each border as a filled rect
    if (box.border.top > 0 && isBorderVisible("border-top-style")) {
        auto c = getBorderColor("border-top-color");
        renderer_->fillRect(x, y, w, box.border.top, c);
    }
    if (box.border.bottom > 0 && isBorderVisible("border-bottom-style")) {
        auto c = getBorderColor("border-bottom-color");
        renderer_->fillRect(x, y + h - box.border.bottom, w, box.border.bottom, c);
    }
    if (box.border.left > 0 && isBorderVisible("border-left-style")) {
        auto c = getBorderColor("border-left-color");
        renderer_->fillRect(x, y + box.border.top, box.border.left,
                           h - box.border.top - box.border.bottom, c);
    }
    if (box.border.right > 0 && isBorderVisible("border-right-style")) {
        auto c = getBorderColor("border-right-color");
        renderer_->fillRect(x + w - box.border.right, y + box.border.top,
                           box.border.right, h - box.border.top - box.border.bottom, c);
    }
}

// Apply text-transform to a string
static std::string applyTextTransform(const std::string& text, const std::string& transform) {
    if (transform == "uppercase") {
        std::string r = text;
        for (auto& c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return r;
    }
    if (transform == "lowercase") {
        std::string r = text;
        for (auto& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return r;
    }
    if (transform == "capitalize") {
        std::string r = text;
        bool nextCap = true;
        for (auto& c : r) {
            if (std::isspace(static_cast<unsigned char>(c))) { nextCap = true; }
            else if (nextCap) { c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); nextCap = false; }
        }
        return r;
    }
    return text;
}

// Parse text-shadow: offsetX offsetY [blur] color (simplified — single shadow only)
struct TextShadow { float dx = 0, dy = 0, blur = 0; render::Color color = {0,0,0,128}; };
static bool parseTextShadow(const std::string& val, TextShadow& out) {
    if (val.empty() || val == "none") return false;
    // Try to parse numbers and a color from the value
    std::istringstream iss(val);
    std::vector<float> nums;
    std::string colorStr;
    std::string token;
    while (iss >> token) {
        char* end = nullptr;
        float v = std::strtof(token.c_str(), &end);
        // Check if the token is a number (possibly with px suffix)
        if (end != token.c_str() && (*end == '\0' || *end == 'p')) {
            nums.push_back(v);
        } else {
            // Accumulate rest as color
            if (!colorStr.empty()) colorStr += ' ';
            colorStr += token;
        }
    }
    if (nums.size() >= 2) {
        out.dx = nums[0];
        out.dy = nums[1];
        if (nums.size() >= 3) out.blur = nums[2];
        if (!colorStr.empty()) DrawTraversal::tryParseColor(colorStr, out.color);
        return true;
    }
    return false;
}

void DrawTraversal::drawText(dom::Node* textNode, dom::Element* parent,
                             float offsetX, float offsetY) {
    if (!textNode || !parent) return;

    auto* tn = static_cast<dom::TextNode*>(textNode);
    std::string text = tn->data();
    if (text.empty()) return;

    // Skip whitespace-only text
    bool allWhitespace = true;
    for (char c : text) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            allWhitespace = false;
            break;
        }
    }
    if (allWhitespace) return;

    auto& style = parent->computedStyle();
    uint64_t fontHandle = getFontHandle(parent);
    if (!fontHandle) return;

    auto metrics = fontManager_->getMetrics(fontHandle);
    float ascent = metrics.ascent;
    float descent = metrics.descent;
    float lineH = metrics.height;

    // Apply text-transform
    auto ttIt = style.find("text-transform");
    if (ttIt != style.end()) text = applyTextTransform(text, ttIt->second);

    // Get text color
    render::Color color = {0, 0, 0, 255};
    auto cIt = style.find("color");
    if (cIt != style.end()) tryParseColor(cIt->second, color);

    // Parse text-shadow
    TextShadow shadow;
    bool hasShadow = false;
    auto tsIt = style.find("text-shadow");
    if (tsIt != style.end()) hasShadow = parseTextShadow(tsIt->second, shadow);

    // Parse text-decoration
    std::string decoration;
    auto tdIt = style.find("text-decoration");
    if (tdIt != style.end()) decoration = tdIt->second;
    if (decoration.empty()) {
        auto tdlIt = style.find("text-decoration-line");
        if (tdlIt != style.end()) decoration = tdlIt->second;
    }

    // Text-align: compute offset when layout provides a content width
    float textAlignOffset = 0;
    auto taIt = style.find("text-align");
    if (taIt != style.end() && (taIt->second == "center" || taIt->second == "right")) {
        auto& pbox = parent->layoutBox();
        float availW = pbox.contentRect.width;
        if (availW > 0) {
            auto tm = renderer_->measureText(text, fontHandle);
            if (taIt->second == "center")
                textAlignOffset = (availW - tm.width) / 2.0f;
            else
                textAlignOffset = availW - tm.width;
            if (textAlignOffset < 0) textAlignOffset = 0;
        }
    }

    // Use layout-computed position if available (from IFC text positioning),
    // otherwise fall back to parent's content origin.
    // When the IFC provides positions, text-align is already applied in contentRect.x,
    // so only add textAlignOffset in the fallback path to avoid double-centering.
    float x, y;

    auto& tbox = tn->layoutBox();
    if (tbox.contentRect.width > 0) {
        x = offsetX + tbox.contentRect.x;
        y = offsetY + tbox.contentRect.y + ascent;
    } else {
        x = offsetX + textAlignOffset;
        y = offsetY + ascent;
    }

    // Helper to draw a single line of text with shadow and decoration
    auto drawLine = [&](std::string_view line, float lx, float ly) {
        auto tm = renderer_->measureText(line, fontHandle);

        // Draw text shadow first (behind text)
        if (hasShadow) {
            renderer_->drawText(line, lx + shadow.dx, ly + shadow.dy, fontHandle, shadow.color);
        }

        // Draw the text
        renderer_->drawText(line, lx, ly, fontHandle, color);

        // Draw text-decoration
        if (!decoration.empty() && decoration != "none") {
            float decoThickness = std::max(1.0f, ascent / 12.0f);
            if (decoration.find("underline") != std::string::npos) {
                float uy = ly + descent * 0.4f;
                renderer_->drawLine(lx, uy, lx + tm.width, uy, color, decoThickness);
            }
            if (decoration.find("overline") != std::string::npos) {
                float oy = ly - ascent;
                renderer_->drawLine(lx, oy, lx + tm.width, oy, color, decoThickness);
            }
            if (decoration.find("line-through") != std::string::npos) {
                float sy = ly - ascent * 0.35f;
                renderer_->drawLine(lx, sy, lx + tm.width, sy, color, decoThickness);
            }
        }
    };

    // Handle multi-line text (newlines in pre/pre-wrap)
    auto wsIt = style.find("white-space");
    bool preserveNewlines = false;
    if (wsIt != style.end()) {
        const auto& ws = wsIt->second;
        preserveNewlines = (ws == "pre" || ws == "pre-wrap" || ws == "pre-line");
    }

    if (preserveNewlines && text.find('\n') != std::string::npos) {
        float curY = y;
        size_t start = 0;
        while (start < text.size()) {
            size_t nl = text.find('\n', start);
            if (nl == std::string::npos) nl = text.size();
            if (nl > start) {
                std::string_view line(text.data() + start, nl - start);
                drawLine(line, x, curY);
            }
            curY += lineH;
            start = nl + 1;
        }
    } else {
        drawLine(text, x, y);
    }
}

uint64_t DrawTraversal::getFontHandle(dom::Element* elem) {
    auto& style = elem->computedStyle();

    std::string family = "Arial";
    auto famIt = style.find("font-family");
    if (famIt != style.end() && !famIt->second.empty()) {
        family = famIt->second;
        // Remove quotes
        if (family.front() == '"' || family.front() == '\'') {
            family = family.substr(1, family.size() - 2);
        }
    }

    float size = 16.0f;
    auto sizeIt = style.find("font-size");
    if (sizeIt != style.end()) {
        char* end = nullptr;
        float v = std::strtof(sizeIt->second.c_str(), &end);
        if (end != sizeIt->second.c_str() && v > 0) size = v;
    }

    int weight = 400;
    auto weightIt = style.find("font-weight");
    if (weightIt != style.end()) {
        const auto& w = weightIt->second;
        if (w == "bold") weight = 700;
        else if (w == "lighter") weight = 100;
        else {
            try { weight = std::stoi(w); } catch (...) {}
        }
    }

    bool italic = false;
    auto styleIt = style.find("font-style");
    if (styleIt != style.end()) {
        italic = (styleIt->second == "italic" || styleIt->second == "oblique");
    }

    return fontManager_->createFont(renderer_, family, size, weight, italic);
}

void DrawTraversal::loadImage(const std::string& url, const std::string& basePath) {
    if (imageCache_.count(url)) return;

    // Strip URL query/fragment so `thumbnails/foo.png?v=12345` (standard
    // cache-bust) resolves to the file on disk.
    std::string cleanUrl = url;
    auto qPos = cleanUrl.find_first_of("?#");
    if (qPos != std::string::npos) cleanUrl.resize(qPos);

    std::string path;
    if (cleanUrl.size() >= 2 && cleanUrl[1] == ':') {
        path = cleanUrl;
    } else if (!cleanUrl.empty() && (cleanUrl[0] == '/' || cleanUrl[0] == '\\')) {
        path = cleanUrl;
    } else if (!basePath.empty()) {
        path = basePath;
        if (path.back() != '/' && path.back() != '\\') path += '/';
        path += cleanUrl;
    } else {
        path = cleanUrl;
    }

    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) {
        LOG_WARN("loadImage: failed to open '%s'", path.c_str());
        imageCache_[url] = CachedImage{};  // negative-cache so we don't re-warn each frame
        return;
    }
    auto fileSize = ifs.tellg();
    ifs.seekg(0);
    CachedImage img;
    img.data.resize(static_cast<size_t>(fileSize));
    ifs.read(reinterpret_cast<char*>(img.data.data()), fileSize);

    int w = 0, h = 0, comp = 0;
    if (stbi_info_from_memory(img.data.data(), static_cast<int>(img.data.size()), &w, &h, &comp)) {
        img.width = w;
        img.height = h;
    }
    imageCache_[url] = std::move(img);
}

bool DrawTraversal::tryParseColor(const std::string& colorStr, render::Color& out) {
    if (colorStr.empty() || colorStr == "transparent") return false;

    // Hex color
    if (colorStr[0] == '#') {
        std::string hex = colorStr.substr(1);
        if (hex.size() == 3) {
            hex = {hex[0], hex[0], hex[1], hex[1], hex[2], hex[2]};
        }
        if (hex.size() == 6 || hex.size() == 8) {
            unsigned long val = std::strtoul(hex.c_str(), nullptr, 16);
            if (hex.size() == 6) {
                out.r = (val >> 16) & 0xFF;
                out.g = (val >> 8) & 0xFF;
                out.b = val & 0xFF;
                out.a = 255;
            } else {
                out.r = (val >> 24) & 0xFF;
                out.g = (val >> 16) & 0xFF;
                out.b = (val >> 8) & 0xFF;
                out.a = val & 0xFF;
            }
            return true;
        }
    }

    // rgb/rgba
    if (colorStr.substr(0, 4) == "rgb(" || colorStr.substr(0, 5) == "rgba(") {
        auto start = colorStr.find('(');
        auto end = colorStr.rfind(')');
        if (start != std::string::npos && end != std::string::npos) {
            std::string inner = colorStr.substr(start + 1, end - start - 1);
            for (char& c : inner) { if (c == ',' || c == '/') c = ' '; }
            std::istringstream iss(inner);
            float r, g, b, a = 1.0f;
            if (iss >> r >> g >> b) {
                iss >> a;
                out.r = static_cast<uint8_t>(std::clamp(r, 0.0f, 255.0f));
                out.g = static_cast<uint8_t>(std::clamp(g, 0.0f, 255.0f));
                out.b = static_cast<uint8_t>(std::clamp(b, 0.0f, 255.0f));
                // If alpha is <= 1.0, treat as 0-1 range; otherwise as 0-255
                if (a <= 1.0f) a *= 255.0f;
                out.a = static_cast<uint8_t>(std::clamp(a, 0.0f, 255.0f));
                return true;
            }
        }
    }

    // hsl/hsla
    if (colorStr.substr(0, 4) == "hsl(" || colorStr.substr(0, 5) == "hsla(") {
        auto start = colorStr.find('(');
        auto end = colorStr.rfind(')');
        if (start != std::string::npos && end != std::string::npos) {
            std::string inner = colorStr.substr(start + 1, end - start - 1);
            // Remove % signs, replace commas/slashes with spaces
            for (char& c : inner) {
                if (c == ',' || c == '/' || c == '%') c = ' ';
            }
            std::istringstream iss(inner);
            float h, s, l, a = 1.0f;
            if (iss >> h >> s >> l) {
                iss >> a;
                // Normalize: h in [0,360), s and l in [0,1]
                h = std::fmod(h, 360.0f);
                if (h < 0) h += 360.0f;
                s = std::clamp(s / 100.0f, 0.0f, 1.0f);
                l = std::clamp(l / 100.0f, 0.0f, 1.0f);
                if (a <= 1.0f) a *= 255.0f;

                auto hue2rgb = [](float p, float q, float t) -> float {
                    if (t < 0) t += 1; if (t > 1) t -= 1;
                    if (t < 1.0f/6) return p + (q-p)*6*t;
                    if (t < 1.0f/2) return q;
                    if (t < 2.0f/3) return p + (q-p)*(2.0f/3-t)*6;
                    return p;
                };
                if (s == 0) {
                    uint8_t v = static_cast<uint8_t>(l * 255);
                    out.r = out.g = out.b = v;
                } else {
                    float q = l < 0.5f ? l*(1+s) : l+s-l*s;
                    float p = 2*l-q;
                    float hn = h/360.0f;
                    out.r = static_cast<uint8_t>(hue2rgb(p, q, hn+1.0f/3)*255);
                    out.g = static_cast<uint8_t>(hue2rgb(p, q, hn)*255);
                    out.b = static_cast<uint8_t>(hue2rgb(p, q, hn-1.0f/3)*255);
                }
                out.a = static_cast<uint8_t>(std::clamp(a, 0.0f, 255.0f));
                return true;
            }
        }
    }

    // Named colors — full CSS Color Level 4 set
    static const std::unordered_map<std::string, render::Color> named = {
        {"aliceblue",{240,248,255,255}},{"antiquewhite",{250,235,215,255}},
        {"aqua",{0,255,255,255}},{"aquamarine",{127,255,212,255}},
        {"azure",{240,255,255,255}},{"beige",{245,245,220,255}},
        {"bisque",{255,228,196,255}},{"black",{0,0,0,255}},
        {"blanchedalmond",{255,235,205,255}},{"blue",{0,0,255,255}},
        {"blueviolet",{138,43,226,255}},{"brown",{165,42,42,255}},
        {"burlywood",{222,184,135,255}},{"cadetblue",{95,158,160,255}},
        {"chartreuse",{127,255,0,255}},{"chocolate",{210,105,30,255}},
        {"coral",{255,127,80,255}},{"cornflowerblue",{100,149,237,255}},
        {"cornsilk",{255,248,220,255}},{"crimson",{220,20,60,255}},
        {"cyan",{0,255,255,255}},{"darkblue",{0,0,139,255}},
        {"darkcyan",{0,139,139,255}},{"darkgoldenrod",{184,134,11,255}},
        {"darkgray",{169,169,169,255}},{"darkgreen",{0,100,0,255}},
        {"darkgrey",{169,169,169,255}},{"darkkhaki",{189,183,107,255}},
        {"darkmagenta",{139,0,139,255}},{"darkolivegreen",{85,107,47,255}},
        {"darkorange",{255,140,0,255}},{"darkorchid",{153,50,204,255}},
        {"darkred",{139,0,0,255}},{"darksalmon",{233,150,122,255}},
        {"darkseagreen",{143,188,143,255}},{"darkslateblue",{72,61,139,255}},
        {"darkslategray",{47,79,79,255}},{"darkslategrey",{47,79,79,255}},
        {"darkturquoise",{0,206,209,255}},{"darkviolet",{148,0,211,255}},
        {"deeppink",{255,20,147,255}},{"deepskyblue",{0,191,255,255}},
        {"dimgray",{105,105,105,255}},{"dimgrey",{105,105,105,255}},
        {"dodgerblue",{30,144,255,255}},{"firebrick",{178,34,34,255}},
        {"floralwhite",{255,250,240,255}},{"forestgreen",{34,139,34,255}},
        {"fuchsia",{255,0,255,255}},{"gainsboro",{220,220,220,255}},
        {"ghostwhite",{248,248,255,255}},{"gold",{255,215,0,255}},
        {"goldenrod",{218,165,32,255}},{"gray",{128,128,128,255}},
        {"green",{0,128,0,255}},{"greenyellow",{173,255,47,255}},
        {"grey",{128,128,128,255}},{"honeydew",{240,255,240,255}},
        {"hotpink",{255,105,180,255}},{"indianred",{205,92,92,255}},
        {"indigo",{75,0,130,255}},{"ivory",{255,255,240,255}},
        {"khaki",{240,230,140,255}},{"lavender",{230,230,250,255}},
        {"lavenderblush",{255,240,245,255}},{"lawngreen",{124,252,0,255}},
        {"lemonchiffon",{255,250,205,255}},{"lightblue",{173,216,230,255}},
        {"lightcoral",{240,128,128,255}},{"lightcyan",{224,255,255,255}},
        {"lightgoldenrodyellow",{250,250,210,255}},{"lightgray",{211,211,211,255}},
        {"lightgreen",{144,238,144,255}},{"lightgrey",{211,211,211,255}},
        {"lightpink",{255,182,193,255}},{"lightsalmon",{255,160,122,255}},
        {"lightseagreen",{32,178,170,255}},{"lightskyblue",{135,206,250,255}},
        {"lightslategray",{119,136,153,255}},{"lightslategrey",{119,136,153,255}},
        {"lightsteelblue",{176,196,222,255}},{"lightyellow",{255,255,224,255}},
        {"lime",{0,255,0,255}},{"limegreen",{50,205,50,255}},
        {"linen",{250,240,230,255}},{"magenta",{255,0,255,255}},
        {"maroon",{128,0,0,255}},{"mediumaquamarine",{102,205,170,255}},
        {"mediumblue",{0,0,205,255}},{"mediumorchid",{186,85,211,255}},
        {"mediumpurple",{147,111,219,255}},{"mediumseagreen",{60,179,113,255}},
        {"mediumslateblue",{123,104,238,255}},{"mediumspringgreen",{0,250,154,255}},
        {"mediumturquoise",{72,209,204,255}},{"mediumvioletred",{199,21,133,255}},
        {"midnightblue",{25,25,112,255}},{"mintcream",{245,255,250,255}},
        {"mistyrose",{255,228,225,255}},{"moccasin",{255,228,181,255}},
        {"navajowhite",{255,222,173,255}},{"navy",{0,0,128,255}},
        {"oldlace",{253,245,230,255}},{"olive",{128,128,0,255}},
        {"olivedrab",{107,142,35,255}},{"orange",{255,165,0,255}},
        {"orangered",{255,69,0,255}},{"orchid",{218,112,214,255}},
        {"palegoldenrod",{238,232,170,255}},{"palegreen",{152,251,152,255}},
        {"paleturquoise",{175,238,238,255}},{"palevioletred",{219,112,147,255}},
        {"papayawhip",{255,239,213,255}},{"peachpuff",{255,218,185,255}},
        {"peru",{205,133,63,255}},{"pink",{255,192,203,255}},
        {"plum",{221,160,221,255}},{"powderblue",{176,224,230,255}},
        {"purple",{128,0,128,255}},{"rebeccapurple",{102,51,153,255}},
        {"red",{255,0,0,255}},{"rosybrown",{188,143,143,255}},
        {"royalblue",{65,105,225,255}},{"saddlebrown",{139,69,19,255}},
        {"salmon",{250,128,114,255}},{"sandybrown",{244,164,96,255}},
        {"seagreen",{46,139,87,255}},{"seashell",{255,245,238,255}},
        {"sienna",{160,82,45,255}},{"silver",{192,192,192,255}},
        {"skyblue",{135,206,235,255}},{"slateblue",{106,90,205,255}},
        {"slategray",{112,128,144,255}},{"slategrey",{112,128,144,255}},
        {"snow",{255,250,250,255}},{"springgreen",{0,255,127,255}},
        {"steelblue",{70,130,180,255}},{"tan",{210,180,140,255}},
        {"teal",{0,128,128,255}},{"thistle",{216,191,216,255}},
        {"tomato",{255,99,71,255}},{"turquoise",{64,224,208,255}},
        {"violet",{238,130,238,255}},{"wheat",{245,222,179,255}},
        {"white",{255,255,255,255}},{"whitesmoke",{245,245,245,255}},
        {"yellow",{255,255,0,255}},{"yellowgreen",{154,205,50,255}},
    };
    std::string lower = colorStr;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto it = named.find(lower);
    if (it != named.end()) {
        out = it->second;
        return true;
    }

    return false;
}

render::Color DrawTraversal::parseColor(const std::string& color) {
    render::Color c = {0, 0, 0, 255};
    tryParseColor(color, c);
    return c;
}

} // namespace bro::layout
