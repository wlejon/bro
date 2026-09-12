#if BRO_WITH_3D

#include "bronze_host/host_scene_internal.h"
#include "bronze_host/host_internal.h"
#include "scene/scene_graph.h"
#include "scene/scene_node.h"
#include "scene/shape_node.h"
#include "scene/sprite_node.h"
#include "scene/html_node.h"

#include <cmath>
#include <span>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

double numAtProp(Value obj, const char* key, double defVal) {
    if (!ev::isObject(obj)) return defVal;
    Value v = ev::getProperty(obj, key);
    return ev::isNumber(v) ? ev::toDouble(v) : defVal;
}

std::string strAtProp(Value obj, const char* key, const std::string& defVal) {
    if (!ev::isObject(obj)) return defVal;
    Value v = ev::getProperty(obj, key);
    return ev::isString(v) ? ev::toUtf8(v) : defVal;
}

bool boolAtProp(Value obj, const char* key, bool defVal) {
    if (!ev::isObject(obj)) return defVal;
    Value v = ev::getProperty(obj, key);
    return !ev::isUndefined(v) ? ev::toBool(v) : defVal;
}

void applyWorldAnchorAndBillboard(Value opts, scene::SceneNode* node) {
    if (!node || !ev::isObject(opts)) return;
    Value wa = ev::getProperty(opts, "worldAnchor");
    if (ev::isObject(wa)) {
        bromath::Vec3 a;
        if (readVec3FromValue(wa, a)) node->setWorldAnchor(a);
    }
    Value bb = ev::getProperty(opts, "billboard");
    if (ev::isString(bb)) {
        std::string mode = ev::toUtf8(bb);
        if (mode == "ylock" || mode == "yLock" || mode == "y-lock") {
            node->setBillboardMode(scene::SceneNode::BillboardMode::YLock);
        } else {
            node->setBillboardMode(scene::SceneNode::BillboardMode::Full);
        }
    }
}

bool extractTextureObj(Value texObj, std::vector<uint8_t>& outBytes, int& outW, int& outH) {
    if (!ev::isObject(texObj)) return false;
    outW = static_cast<int>(numAtProp(texObj, "width", 0));
    outH = static_cast<int>(numAtProp(texObj, "height", 0));
    Value dVal = ev::getProperty(texObj, "data");
    ev::TypedArrayInfo info = ev::typedArrayInfo(dVal);
    if (!info || !info.data || info.byteLength < static_cast<size_t>(outW * outH * 4)) return false;
    outBytes.assign(reinterpret_cast<const uint8_t*>(info.data), reinterpret_cast<const uint8_t*>(info.data) + info.byteLength);
    return true;
}

static scene::SpriteNode::AnimationSpec parseAnimSpec(Value specVal) {
    scene::SpriteNode::AnimationSpec spec;
    Value framesVal = ev::getProperty(specVal, "frames");
    if (ev::isObject(framesVal)) {
        Value lenVal = ev::getProperty(framesVal, "length");
        if (ev::isNumber(lenVal)) {
            int len = static_cast<int>(ev::toDouble(lenVal));
            for (int i = 0; i < len; ++i) {
                Value fVal = ev::getElement(framesVal, i);
                if (ev::isNumber(fVal)) spec.frames.push_back(static_cast<int>(ev::toDouble(fVal)));
            }
        }
    }
    spec.fps = static_cast<float>(numAtProp(specVal, "fps", 12.0));
    spec.loop = boolAtProp(specVal, "loop", true);
    spec.next = strAtProp(specVal, "next", "");
    return spec;
}

static void applySpriteSheet(Value opts, scene::SpriteNode* node) {
    Value sheetVal = ev::getProperty(opts, "sheet");
    if (!ev::isObject(sheetVal)) return;

    Value framesVal = ev::getProperty(sheetVal, "frames");
    if (ev::isObject(framesVal)) {
        Value lenVal = ev::getProperty(framesVal, "length");
        if (ev::isNumber(lenVal)) {
            int len = static_cast<int>(ev::toDouble(lenVal));
            std::vector<scene::SpriteNode::Frame> frames;
            frames.reserve(len);
            for (int i = 0; i < len; ++i) {
                Value f = ev::getElement(framesVal, i);
                scene::SpriteNode::Frame fr{};
                fr.x = static_cast<float>(numAtProp(f, "x", 0));
                fr.y = static_cast<float>(numAtProp(f, "y", 0));
                fr.w = static_cast<float>(numAtProp(f, "w", 0));
                fr.h = static_cast<float>(numAtProp(f, "h", 0));
                frames.push_back(fr);
            }
            node->setSheetFrames(std::move(frames));
            return;
        }
    }
    int fw = static_cast<int>(numAtProp(sheetVal, "frameWidth", 0));
    int fh = static_cast<int>(numAtProp(sheetVal, "frameHeight", 0));
    int cols = static_cast<int>(numAtProp(sheetVal, "columns", 1));
    int rows = static_cast<int>(numAtProp(sheetVal, "rows", 1));
    if (fw > 0 && fh > 0) {
        node->setSheetGrid(fw, fh, cols, rows);
    }
}

static void applySpriteAnimations(Value opts, scene::SpriteNode* node) {
    Value animsVal = ev::getProperty(opts, "animations");
    if (!ev::isObject(animsVal)) return;

    Value objCtor = ev::globalValue("Object").value;
    Value keysFn = ev::getProperty(objCtor, "keys");
    Value keysArr = ev::call(keysFn, objCtor, std::span<const Value>(&animsVal, 1)).value;
    Value lenVal = ev::getProperty(keysArr, "length");
    uint32_t len = ev::isNumber(lenVal) ? static_cast<uint32_t>(ev::toDouble(lenVal)) : 0;
    for (uint32_t i = 0; i < len; ++i) {
        Value kVal = ev::getElement(keysArr, i);
        std::string key = ev::toUtf8(kVal);
        Value specVal = ev::getProperty(animsVal, key.c_str());
        if (ev::isObject(specVal)) {
            node->addAnimation(key, parseAnimSpec(specVal));
        }
    }
}

}  // namespace

void installSceneGraph2D(ObjectBuilder& b) {
    b.def("createShape", 1, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (!g) return ev::undefined();
        auto* node = g->createShape();
        g->root()->addChild(node);
        if (!a.empty() && ev::isObject(a[0])) {
            Value opts = a[0];
            Value nameVal = ev::getProperty(opts, "name");
            if (ev::isString(nameVal)) node->setName(ev::toUtf8(nameVal));

            std::string sh = strAtProp(opts, "shape", "rect");
            if (sh == "circle") node->setShape(scene::ShapeNode::Shape::Circle);
            else if (sh == "roundRect" || sh == "roundrect") node->setShape(scene::ShapeNode::Shape::RoundRect);
            else node->setShape(scene::ShapeNode::Shape::Rect);

            double w = numAtProp(opts, "width", 32);
            double h = numAtProp(opts, "height", 32);
            node->setSize((float)w, (float)h);

            double rad = numAtProp(opts, "radius", 0);
            node->setRadius((float)rad);

            Value fc = ev::getProperty(opts, "fillColor");
            float fr = 1, fg = 1, fb = 1, fa = 1;
            if (parseColorValue(fc, fr, fg, fb, fa)) node->setFillColor(bromath::Color{fr, fg, fb, fa});

            Value sc = ev::getProperty(opts, "strokeColor");
            float sr = 0, sg = 0, sb = 0, sa = 1;
            if (parseColorValue(sc, sr, sg, sb, sa)) node->setStrokeColor(bromath::Color{sr, sg, sb, sa});

            double sw = numAtProp(opts, "strokeWidth", 0);
            node->setStrokeWidth((float)sw);

            applyWorldAnchorAndBillboard(opts, node);

            double x = numAtProp(opts, "x", 0);
            double y = numAtProp(opts, "y", 0);
            double z = numAtProp(opts, "z", 0);
            node->setPosition((float)x, (float)y, (float)z);
        }
        return wrapSceneNode(node, g);
    });

    b.def("createSprite", 1, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (!g) return ev::undefined();
        auto* node = g->createSprite();
        g->root()->addChild(node);
        if (!a.empty() && ev::isObject(a[0])) {
            Value opts = a[0];
            Value nameVal = ev::getProperty(opts, "name");
            if (ev::isString(nameVal)) node->setName(ev::toUtf8(nameVal));

            Value srcVal = ev::getProperty(opts, "src");
            if (ev::isString(srcVal)) node->setImagePath(ev::toUtf8(srcVal));

            double w = numAtProp(opts, "width", 32);
            double h = numAtProp(opts, "height", 32);
            node->setSize((float)w, (float)h);

            Value imgVal = ev::getProperty(opts, "image");
            std::vector<uint8_t> bytes;
            int iw = 0, ih = 0;
            if (extractTextureObj(imgVal, bytes, iw, ih)) {
                node->setImageData(bytes.data(), iw, ih);
            }

            double fw = numAtProp(opts, "frameWidth", 0);
            double fh = numAtProp(opts, "frameHeight", 0);
            double cols = numAtProp(opts, "columns", 1);
            double rows = numAtProp(opts, "rows", 1);
            if (fw > 0 && fh > 0) {
                node->setSheetGrid(static_cast<int>(fw), static_cast<int>(fh), static_cast<int>(cols), static_cast<int>(rows));
            }

            applySpriteSheet(opts, node);
            applySpriteAnimations(opts, node);

            Value playVal = ev::getProperty(opts, "play");
            if (ev::isString(playVal)) node->play(ev::toUtf8(playVal));

            Value fiVal = ev::getProperty(opts, "frameIndex");
            if (ev::isNumber(fiVal)) node->setFrameIndex(static_cast<int>(ev::toDouble(fiVal)));

            applyWorldAnchorAndBillboard(opts, node);

            double x = numAtProp(opts, "x", 0);
            double y = numAtProp(opts, "y", 0);
            double z = numAtProp(opts, "z", 0);
            node->setPosition((float)x, (float)y, (float)z);
        }
        return wrapSceneNode(node, g);
    });

    auto makeHtml = [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (!g) return ev::undefined();
        auto* node = g->createHtml();
        g->root()->addChild(node);
        if (!a.empty() && ev::isObject(a[0])) {
            Value opts = a[0];
            Value nameVal = ev::getProperty(opts, "name");
            if (ev::isString(nameVal)) node->setName(ev::toUtf8(nameVal));

            Value htmlVal = ev::getProperty(opts, "html");
            if (ev::isString(htmlVal)) node->setHtml(ev::toUtf8(htmlVal));

            double ppu = numAtProp(opts, "pixelsPerUnit", 100);
            node->setPxPerUnit((float)ppu);

            double dmin = numAtProp(opts, "distanceMin", 0);
            double dmax = numAtProp(opts, "distanceMax", 1000);
            node->setVisibilityRange((float)dmin, (float)dmax);

            applyWorldAnchorAndBillboard(opts, node);

            double x = numAtProp(opts, "x", 0);
            double y = numAtProp(opts, "y", 0);
            double z = numAtProp(opts, "z", 0);
            node->setPosition((float)x, (float)y, (float)z);
        }
        return wrapSceneNode(node, g);
    };
    b.def("createHtml", 1, makeHtml);
    b.def("createHtmlNode", 1, makeHtml);
}

void installSceneNode2D(ObjectBuilder& b) {
    // Shape & Sprite properties
    b.accessor("width",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (n) {
                if (n->type() == scene::SceneNode::Type::Shape)
                    return ev::fromDouble(static_cast<scene::ShapeNode*>(n)->width());
                if (n->type() == scene::SceneNode::Type::Sprite)
                    return ev::fromDouble(static_cast<scene::SpriteNode*>(n)->width());
            }
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && !a.empty() && ev::isNumber(a[0])) {
                float w = static_cast<float>(ev::toDouble(a[0]));
                if (n->type() == scene::SceneNode::Type::Shape) {
                    auto* s = static_cast<scene::ShapeNode*>(n);
                    s->setSize(w, s->height());
                } else if (n->type() == scene::SceneNode::Type::Sprite) {
                    auto* s = static_cast<scene::SpriteNode*>(n);
                    s->setSize(w, s->height());
                }
            }
            return ev::undefined();
        });

    b.accessor("height",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (n) {
                if (n->type() == scene::SceneNode::Type::Shape)
                    return ev::fromDouble(static_cast<scene::ShapeNode*>(n)->height());
                if (n->type() == scene::SceneNode::Type::Sprite)
                    return ev::fromDouble(static_cast<scene::SpriteNode*>(n)->height());
            }
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && !a.empty() && ev::isNumber(a[0])) {
                float h = static_cast<float>(ev::toDouble(a[0]));
                if (n->type() == scene::SceneNode::Type::Shape) {
                    auto* s = static_cast<scene::ShapeNode*>(n);
                    s->setSize(s->width(), h);
                } else if (n->type() == scene::SceneNode::Type::Sprite) {
                    auto* s = static_cast<scene::SpriteNode*>(n);
                    s->setSize(s->width(), h);
                }
            }
            return ev::undefined();
        });

    b.accessor("radius",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Shape)
                return ev::fromDouble(static_cast<scene::ShapeNode*>(n)->radius());
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Shape && !a.empty() && ev::isNumber(a[0]))
                static_cast<scene::ShapeNode*>(n)->setRadius(static_cast<float>(ev::toDouble(a[0])));
            return ev::undefined();
        });

    b.accessor("fillColor",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Shape) {
                const auto& c = static_cast<scene::ShapeNode*>(n)->fillColor();
                return hostArrayOf(4, [&c](size_t i) {
                    if (i == 0) return ev::fromDouble(c.r);
                    if (i == 1) return ev::fromDouble(c.g);
                    if (i == 2) return ev::fromDouble(c.b);
                    return ev::fromDouble(c.a);
                });
            }
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Shape && !a.empty()) {
                float r = 1, g = 1, b = 1, al = 1;
                if (parseColorValue(a[0], r, g, b, al)) {
                    static_cast<scene::ShapeNode*>(n)->setFillColor(bromath::Color{r, g, b, al});
                }
            }
            return ev::undefined();
        });

    b.accessor("strokeColor",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Shape) {
                const auto& c = static_cast<scene::ShapeNode*>(n)->strokeColor();
                return hostArrayOf(4, [&c](size_t i) {
                    if (i == 0) return ev::fromDouble(c.r);
                    if (i == 1) return ev::fromDouble(c.g);
                    if (i == 2) return ev::fromDouble(c.b);
                    return ev::fromDouble(c.a);
                });
            }
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Shape && !a.empty()) {
                float r = 0, g = 0, b = 0, al = 1;
                if (parseColorValue(a[0], r, g, b, al)) {
                    static_cast<scene::ShapeNode*>(n)->setStrokeColor(bromath::Color{r, g, b, al});
                }
            }
            return ev::undefined();
        });

    b.accessor("strokeWidth",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Shape)
                return ev::fromDouble(static_cast<scene::ShapeNode*>(n)->strokeWidth());
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Shape && !a.empty() && ev::isNumber(a[0]))
                static_cast<scene::ShapeNode*>(n)->setStrokeWidth(static_cast<float>(ev::toDouble(a[0])));
            return ev::undefined();
        });

    // Sprite properties
    b.accessor("frameIndex",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Sprite)
                return ev::fromDouble(static_cast<scene::SpriteNode*>(n)->frameIndex());
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Sprite && !a.empty() && ev::isNumber(a[0]))
                static_cast<scene::SpriteNode*>(n)->setFrameIndex(static_cast<int>(ev::toDouble(a[0])));
            return ev::undefined();
        });

    b.accessor("onAnimationEnd",
        [](Value self_, std::span<const Value>) { return ev::undefined(); },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (!n || n->type() != scene::SceneNode::Type::Sprite) return ev::undefined();
            auto* sp = static_cast<scene::SpriteNode*>(n);
            if (!a.empty() && ev::isFunction(a[0])) {
                auto fnRef = std::make_shared<ev::Persistent>(a[0]);
                sp->setOnAnimationEnd([fnRef](const std::string& name) {
                    if (fnRef && ev::isFunction(fnRef->get())) {
                        Value arg = ev::fromUtf8(name);
                        ev::call(fnRef->get(), ev::undefined(), std::span<const Value>(&arg, 1));
                    }
                });
            } else {
                sp->setOnAnimationEnd(nullptr);
            }
            return ev::undefined();
        });

    b.def("addAnimation", 2, [](Value self_, std::span<const Value> a) {
        auto* n = sceneNodeOf(self_);
        if (n && n->type() == scene::SceneNode::Type::Sprite && a.size() >= 2 && ev::isString(a[0]) && ev::isObject(a[1])) {
            static_cast<scene::SpriteNode*>(n)->addAnimation(ev::toUtf8(a[0]), parseAnimSpec(a[1]));
        }
        return self_;
    });

    // Html properties
    b.accessor("root", [](Value self_, std::span<const Value>) {
        auto* n = sceneNodeOf(self_);
        if (n && n->type() == scene::SceneNode::Type::Html) {
            auto* hn = static_cast<scene::HtmlNode*>(n);
            dom::Element* root = hn->root();
            return root ? hostElementValue(root) : ev::null();
        }
        return ev::undefined();
    }, nullptr);

    b.def("setHtml", 1, [](Value self_, std::span<const Value> a) {
        auto* n = sceneNodeOf(self_);
        if (n && n->type() == scene::SceneNode::Type::Html && !a.empty()) {
            static_cast<scene::HtmlNode*>(n)->setHtml(ev::toUtf8(a[0]));
        }
        return ev::undefined();
    });

    b.def("markHtmlDirty", 0, [](Value self_, std::span<const Value> a) {
        auto* n = sceneNodeOf(self_);
        if (n && n->type() == scene::SceneNode::Type::Html) {
            static_cast<scene::HtmlNode*>(n)->markHtmlDirty();
        }
        return ev::undefined();
    });
}

}  // namespace bro::bronze_host

#endif  // BRO_WITH_3D
