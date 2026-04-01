#include "layout/draw_traversal.h"
#include "layout/el_input.h"
#include "layout/el_textarea.h"
#include "layout/el_select.h"
#include "layout/el_svg.h"
#include "dom/element.h"
#include "dom/text_node.h"
#include "dom/node.h"
#include "dom/shadow_root.h"
#include "util/log.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stb_image.h>

namespace bro::layout {

DrawTraversal::DrawTraversal(render::Renderer* renderer, FontManager* fontManager)
    : renderer_(renderer), fontManager_(fontManager) {}

void DrawTraversal::draw(dom::Element* root, float scrollX, float scrollY,
                         int viewportW, int viewportH) {
    if (!root || !renderer_) return;
    viewportW_ = viewportW;
    viewportH_ = viewportH;
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

    if (visible) {
        // For html/body elements, background covers the entire viewport (CSS2.1 spec)
        std::string tag = elem->tagName();
        if ((tag == "html" || tag == "HTML" || tag == "body" || tag == "BODY") &&
            viewportW_ > 0 && viewportH_ > 0) {
            drawBackground(elem, 0, 0,
                           static_cast<float>(viewportW_), static_cast<float>(viewportH_));
        } else {
            drawBackground(elem, bx, by, bw, bh);
        }

        // Draw borders
        drawBorders(elem, bx, by, bw, bh);
    }

    // Check for overflow clipping
    bool needsClip = false;
    auto ovIt = style.find("overflow");
    std::string overflow = (ovIt != style.end()) ? ovIt->second : "visible";
    if (overflow == "hidden" || overflow == "scroll" || overflow == "auto") {
        needsClip = true;
        renderer_->save();
        // For html/body, clip to viewport (abs-positioned children extend beyond content box)
        std::string tag = elem->tagName();
        if ((tag == "html" || tag == "HTML" || tag == "body" || tag == "BODY") &&
            viewportW_ > 0 && viewportH_ > 0) {
            renderer_->setClip(0, 0, static_cast<float>(viewportW_), static_cast<float>(viewportH_));
        } else {
            renderer_->setClip(bx, by, bw, bh);
        }
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
        return;
    }

    // Children's offset is the parent's absolute content position
    // (so child positions, which are relative to parent content area, become absolute)
    float childOffsetX = x;
    float childOffsetY = y - elem->scrollTopValue();

    // Draw children (handles shadow DOM composed children)
    std::vector<dom::Node*> childNodes;
    if (elem->hasShadow()) {
        auto* sr = elem->shadowRoot();
        if (!sr->slotsValid()) sr->distributeSlots();
        childNodes = sr->composedChildren();
    } else {
        childNodes = elem->childNodes();
    }

    // Get enclosing shadow root for slot replacement on non-host elements
    auto* enclosingSR = elem->containingShadowRoot();

    for (auto* child : childNodes) {
        // Replace <slot> elements with assigned/fallback content
        if (enclosingSR && child->nodeType() == dom::NodeType::Element) {
            auto* childElem = static_cast<dom::Element*>(child);
            if (childElem->tagName() == "SLOT") {
                auto assigned = enclosingSR->assignedNodes(childElem);
                if (!assigned.empty()) {
                    for (auto* n : assigned)
                        drawNode(n, childOffsetX, childOffsetY);
                } else {
                    for (auto* n : childElem->childNodes())
                        drawNode(n, childOffsetX, childOffsetY);
                }
                continue;
            }
        }
        drawNode(child, childOffsetX, childOffsetY);
    }

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
        // SVG is handled above with early return (skips child traversal)
    }

    if (needsClip) {
        renderer_->restore();
    }
}

void DrawTraversal::drawBackground(dom::Element* elem, float x, float y, float w, float h) {
    auto& style = elem->computedStyle();

    // Background color
    auto bgIt = style.find("background-color");
    if (bgIt != style.end() && !bgIt->second.empty()) {
        render::Color c;
        if (tryParseColor(bgIt->second, c) && c.a > 0) {
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
                // Remove quotes
                if (!url.empty() && (url.front() == '"' || url.front() == '\'')) {
                    url = url.substr(1, url.size() - 2);
                }
                loadImage(url, basePath_);
                auto it = imageCache_.find(url);
                if (it != imageCache_.end() && !it->second.data.empty()) {
                    renderer_->drawImage(it->second.data.data(), it->second.data.size(),
                                        x, y, w, h);
                }
            }
        }
        // TODO: linear-gradient, radial-gradient, conic-gradient
    }
}

void DrawTraversal::drawBorders(dom::Element* elem, float x, float y, float w, float h) {
    auto& box = elem->layoutBox();

    auto& style = elem->computedStyle();

    // Top border
    if (box.border.top > 0) {
        auto bsIt = style.find("border-top-style");
        if (bsIt == style.end() || bsIt->second != "none") {
            auto bcIt = style.find("border-top-color");
            render::Color c = {0, 0, 0, 255};
            if (bcIt != style.end()) tryParseColor(bcIt->second, c);
            renderer_->drawLine(x, y, x + w, y, c, box.border.top);
        }
    }

    // Bottom border
    if (box.border.bottom > 0) {
        auto bsIt = style.find("border-bottom-style");
        if (bsIt == style.end() || bsIt->second != "none") {
            auto bcIt = style.find("border-bottom-color");
            render::Color c = {0, 0, 0, 255};
            if (bcIt != style.end()) tryParseColor(bcIt->second, c);
            renderer_->drawLine(x, y + h, x + w, y + h, c, box.border.bottom);
        }
    }

    // Left border
    if (box.border.left > 0) {
        auto bsIt = style.find("border-left-style");
        if (bsIt == style.end() || bsIt->second != "none") {
            auto bcIt = style.find("border-left-color");
            render::Color c = {0, 0, 0, 255};
            if (bcIt != style.end()) tryParseColor(bcIt->second, c);
            renderer_->drawLine(x, y, x, y + h, c, box.border.left);
        }
    }

    // Right border
    if (box.border.right > 0) {
        auto bsIt = style.find("border-right-style");
        if (bsIt == style.end() || bsIt->second != "none") {
            auto bcIt = style.find("border-right-color");
            render::Color c = {0, 0, 0, 255};
            if (bcIt != style.end()) tryParseColor(bcIt->second, c);
            renderer_->drawLine(x + w, y, x + w, y + h, c, box.border.right);
        }
    }
}

void DrawTraversal::drawText(dom::Node* textNode, dom::Element* parent,
                             float offsetX, float offsetY) {
    if (!textNode || !parent) return;

    auto* tn = static_cast<dom::TextNode*>(textNode);
    const std::string& text = tn->data();
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
    float lineH = metrics.height;

    // Get text color
    render::Color color = {0, 0, 0, 255};
    auto cIt = style.find("color");
    if (cIt != style.end()) tryParseColor(cIt->second, color);

    // Use layout-computed position if available (from IFC text positioning),
    // otherwise fall back to parent's content origin
    float x = offsetX;
    float y = offsetY + ascent;

    auto& tbox = tn->layoutBox();
    if (tbox.contentRect.width > 0) {
        x = offsetX + tbox.contentRect.x;
        y = offsetY + tbox.contentRect.y + ascent;
    }

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
                renderer_->drawText(line, x, curY, fontHandle, color);
            }
            curY += lineH;
            start = nl + 1;
        }
    } else {
        renderer_->drawText(text, x, y, fontHandle, color);
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

    std::string path;
    if (url.size() >= 2 && url[1] == ':') {
        path = url;
    } else if (!url.empty() && (url[0] == '/' || url[0] == '\\')) {
        path = url;
    } else if (!basePath.empty()) {
        path = basePath;
        if (path.back() != '/' && path.back() != '\\') path += '/';
        path += url;
    } else {
        path = url;
    }

    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) {
        LOG_WARN("loadImage: failed to open '%s'", path.c_str());
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

    // Named colors (common ones)
    static const std::unordered_map<std::string, render::Color> named = {
        {"black", {0,0,0,255}}, {"white", {255,255,255,255}},
        {"red", {255,0,0,255}}, {"green", {0,128,0,255}},
        {"blue", {0,0,255,255}}, {"yellow", {255,255,0,255}},
        {"gray", {128,128,128,255}}, {"grey", {128,128,128,255}},
        {"silver", {192,192,192,255}}, {"orange", {255,165,0,255}},
        {"purple", {128,0,128,255}}, {"pink", {255,192,203,255}},
        {"brown", {165,42,42,255}}, {"cyan", {0,255,255,255}},
        {"magenta", {255,0,255,255}}, {"lime", {0,255,0,255}},
        {"navy", {0,0,128,255}}, {"teal", {0,128,128,255}},
        {"maroon", {128,0,0,255}}, {"olive", {128,128,0,255}},
        {"coral", {255,127,80,255}}, {"gold", {255,215,0,255}},
        {"indigo", {75,0,130,255}}, {"crimson", {220,20,60,255}},
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
