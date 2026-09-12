#if BRO_WITH_3D

#include "bronze_host/host_scene_internal.h"
#include "scene/scene_graph.h"
#include "scene/scene_node.h"
#include "scene/mesh_node.h"
#include "scene/instanced_mesh_node.h"
#include "scene/scene_renderer.h"
#include "util/asset_path.h"

#include <climits>
#include <string>
#include <vector>
#include <span>

namespace bro::bronze_host {

namespace {

static double numAtProp(Value obj, const char* key, double defVal) {
    if (!ev::isObject(obj)) return defVal;
    Value v = ev::getProperty(obj, key);
    return ev::isNumber(v) ? ev::toDouble(v) : defVal;
}

static bool boolAtProp(Value obj, const char* key, bool defVal) {
    if (!ev::isObject(obj)) return defVal;
    Value v = ev::getProperty(obj, key);
    return ev::isUndefined(v) ? defVal : ev::toBool(v);
}

static bool validUserUniformName(const std::string& s) {
    return s.size() > 2 && s[0] == 'u' && s[1] == '_';
}

static bool isShaderableNode(scene::SceneNode* n) {
    if (!n) return false;
    return n->type() == scene::SceneNode::Type::Mesh ||
           n->type() == scene::SceneNode::Type::InstancedMesh;
}

static bool parseUniformValue(Value v, std::vector<float>& outFloats) {
    if (ev::isNumber(v)) {
        outFloats.push_back(static_cast<float>(ev::toDouble(v)));
        return true;
    }
    if (ev::isObject(v)) {
        Value lenVal = ev::getProperty(v, "length");
        if (ev::isNumber(lenVal)) {
            int len = static_cast<int>(ev::toDouble(lenVal));
            if (len < 1 || len > 4) return false;
            for (int i = 0; i < len; ++i) {
                Value elem = ev::getElement(v, i);
                if (!ev::isNumber(elem)) return false;
                outFloats.push_back(static_cast<float>(ev::toDouble(elem)));
            }
            return true;
        }
    }
    return false;
}

} // namespace

void installSceneShader(ObjectBuilder& bNode, ObjectBuilder& bGraph) {
    // --- SceneNode custom shader methods & accessors ---

    bNode.accessor("hasShader", [](Value self_, std::span<const Value>) {
        auto* n = sceneNodeOf(self_);
        if (!isShaderableNode(n)) return ev::fromBool(false);
        if (n->type() == scene::SceneNode::Type::InstancedMesh)
            return ev::fromBool(static_cast<scene::InstancedMeshNode*>(n)->hasCustomShader());
        return ev::fromBool(static_cast<scene::MeshNode*>(n)->hasCustomShader());
    }, nullptr);

    bNode.accessor("cullMargin",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (!isShaderableNode(n)) return ev::undefined();
            if (n->type() == scene::SceneNode::Type::InstancedMesh)
                return ev::fromDouble(static_cast<scene::InstancedMeshNode*>(n)->cullMargin());
            return ev::fromDouble(static_cast<scene::MeshNode*>(n)->cullMargin());
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (!isShaderableNode(n) || a.empty() || !ev::isNumber(a[0])) return ev::undefined();
            float m = static_cast<float>(ev::toDouble(a[0]));
            if (m < 0.0f) m = 0.0f;
            if (n->type() == scene::SceneNode::Type::InstancedMesh)
                static_cast<scene::InstancedMeshNode*>(n)->setCullMargin(m);
            else
                static_cast<scene::MeshNode*>(n)->setCullMargin(m);
            return ev::undefined();
        });

    bNode.def("setShader", 1, [](Value self_, std::span<const Value> a) -> Value {
        auto* cell = sceneNodeCellOf(self_);
        auto* n = cell ? cell->node() : nullptr;
        auto* g = cell ? cell->graph() : nullptr;
        if (!isShaderableNode(n) || !g)
            return ev::throwTypeError("setShader: not a MeshNode or InstancedMeshNode");
        if (a.empty() || !ev::isObject(a[0]))
            return ev::throwTypeError("setShader: expected { vertex?, fragment?, uniforms? }");

        Value opts = a[0];
        std::string vertex, fragment;
        Value vVal = ev::getProperty(opts, "vertex");
        Value fVal = ev::getProperty(opts, "fragment");
        if (!ev::isUndefined(vVal)) {
            if (!ev::isString(vVal)) return ev::throwTypeError("setShader: vertex must be a GLSL string");
            vertex = ev::toUtf8(vVal);
        }
        if (!ev::isUndefined(fVal)) {
            if (!ev::isString(fVal)) return ev::throwTypeError("setShader: fragment must be a GLSL string");
            fragment = ev::toUtf8(fVal);
        }
        if (vertex.empty() && fragment.empty())
            return ev::throwTypeError("setShader: at least one of vertex/fragment is required");

        std::vector<std::pair<std::string, std::vector<float>>> uniforms;
        Value uVal = ev::getProperty(opts, "uniforms");
        if (ev::isObject(uVal)) {
            Value objCtor = ev::globalValue("Object").value;
            Value keysFn = ev::getProperty(objCtor, "keys");
            Value keysArr = ev::call(keysFn, objCtor, std::span<const Value>(&uVal, 1)).value;
            Value lenVal = ev::getProperty(keysArr, "length");
            uint32_t len = ev::isNumber(lenVal) ? static_cast<uint32_t>(ev::toDouble(lenVal)) : 0;
            for (uint32_t i = 0; i < len; ++i) {
                Value kVal = ev::getElement(keysArr, i);
                std::string k = ev::toUtf8(kVal);
                if (!validUserUniformName(k))
                    return ev::throwTypeError("setShader: uniform '" + k + "' must use the u_ prefix and be a number or array of numbers");
                Value val = ev::getProperty(uVal, k.c_str());
                std::vector<float> floats;
                if (!parseUniformValue(val, floats))
                    return ev::throwTypeError("setShader: uniform '" + k + "' must use the u_ prefix and be a number or array of numbers");
                uniforms.emplace_back(k, std::move(floats));
            }
        }

        std::string key = vertex + "\x1f" + fragment;
        scene::SceneRenderer::CustomShaderTarget target = scene::SceneRenderer::CustomShaderTarget::Static;
        if (n->type() == scene::SceneNode::Type::InstancedMesh) {
            target = scene::SceneRenderer::CustomShaderTarget::Instanced;
        } else if (static_cast<scene::MeshNode*>(n)->asSkinnedMesh() != nullptr) {
            target = scene::SceneRenderer::CustomShaderTarget::Skinned;
        }

        std::string errOut;
        if (!g->compileCustomShader(target, key, vertex, fragment, errOut)) {
            return ev::throwError("setShader: compilation failed: " + errOut);
        }

        if (n->type() == scene::SceneNode::Type::InstancedMesh) {
            auto* in = static_cast<scene::InstancedMeshNode*>(n);
            in->setCustomShader(vertex, fragment);
            for (const auto& u : uniforms) {
                in->setCustomShaderUniform(u.first, static_cast<int>(u.second.size()), u.second.data());
            }
        } else {
            auto* mn = static_cast<scene::MeshNode*>(n);
            mn->setCustomShader(vertex, fragment);
            for (const auto& u : uniforms) {
                mn->setCustomShaderUniform(u.first, static_cast<int>(u.second.size()), u.second.data());
            }
        }
        return self_;
    });

    bNode.def("setShaderUniform", 2, [](Value self_, std::span<const Value> a) -> Value {
        auto* n = sceneNodeOf(self_);
        if (!isShaderableNode(n))
            return ev::throwTypeError("setShaderUniform: not a MeshNode or InstancedMeshNode");

        bool hasShader = (n->type() == scene::SceneNode::Type::InstancedMesh)
            ? static_cast<scene::InstancedMeshNode*>(n)->hasCustomShader()
            : static_cast<scene::MeshNode*>(n)->hasCustomShader();
        if (!hasShader)
            return ev::throwTypeError("setShaderUniform: no custom shader set (call setShader first)");

        if (a.size() < 2 || !ev::isString(a[0]))
            return ev::throwTypeError("setShaderUniform: expected (name, value)");

        std::string name = ev::toUtf8(a[0]);
        if (!validUserUniformName(name))
            return ev::throwTypeError("setShaderUniform: uniform name must use the u_ prefix (got '" + name + "')");

        std::vector<float> floats;
        if (!parseUniformValue(a[1], floats))
            return ev::throwTypeError("setShaderUniform: value must be a number or an array of 1-4 numbers");

        if (n->type() == scene::SceneNode::Type::InstancedMesh) {
            static_cast<scene::InstancedMeshNode*>(n)->setCustomShaderUniform(
                name, static_cast<int>(floats.size()), floats.data());
        } else {
            static_cast<scene::MeshNode*>(n)->setCustomShaderUniform(
                name, static_cast<int>(floats.size()), floats.data());
        }
        return self_;
    });

    bNode.def("setShaderTexture", 2, [](Value self_, std::span<const Value> a) -> Value {
        auto* n = sceneNodeOf(self_);
        if (!n || n->type() != scene::SceneNode::Type::Mesh)
            return ev::throwTypeError("setShaderTexture: not a MeshNode");
        auto* mn = static_cast<scene::MeshNode*>(n);
        if (!mn->hasCustomShader())
            return ev::throwTypeError("setShaderTexture: no custom shader set (call setShader first)");
        if (a.empty() || !ev::isString(a[0]))
            return ev::throwTypeError("setShaderTexture: expected (name, { width, height, data } | null)");

        std::string name = ev::toUtf8(a[0]);
        if (!validUserUniformName(name))
            return ev::throwTypeError("setShaderTexture: uniform name must use the u_ prefix (got '" + name + "')");

        if (a.size() < 2 || ev::isNull(a[1]) || ev::isUndefined(a[1])) {
            mn->clearCustomShaderTexture(name);
            return self_;
        }
        if (!ev::isObject(a[1]))
            return ev::throwTypeError("setShaderTexture: expected { width, height, data: Float32Array } or null");

        Value opts = a[1];
        int tw = static_cast<int>(numAtProp(opts, "width", 0));
        int th = static_cast<int>(numAtProp(opts, "height", 0));
        if (tw <= 0 || th <= 0)
            return ev::throwTypeError("setShaderTexture: width and height must be positive");

        constexpr int kNoPos = INT_MIN;
        Value xVal = ev::getProperty(opts, "x");
        Value yVal = ev::getProperty(opts, "y");
        int sx = ev::isNumber(xVal) ? static_cast<int>(ev::toDouble(xVal)) : kNoPos;
        int sy = ev::isNumber(yVal) ? static_cast<int>(ev::toDouble(yVal)) : kNoPos;
        bool isSub = (sx != kNoPos || sy != kNoPos);

        int channels = static_cast<int>(numAtProp(opts, "channels", 1));
        if (channels < 1 || channels > 4)
            return ev::throwTypeError("setShaderTexture: channels must be 1..4 (got " + std::to_string(channels) + ")");

        Value dataVal = ev::getProperty(opts, "data");
        auto info = ev::typedArrayInfo(dataVal);
        if (!info)
            return ev::throwTypeError("setShaderTexture: data must be a Float32Array");

        size_t need = static_cast<size_t>(tw) * static_cast<size_t>(th) * static_cast<size_t>(channels) * sizeof(float);
        if (info.byteLength < need)
            return ev::throwTypeError("setShaderTexture: data must hold width*height*channels floats");

        const float* pixels = reinterpret_cast<const float*>(info.data);

        if (isSub) {
            mn->updateCustomShaderTexture(name, sx == kNoPos ? 0 : sx, sy == kNoPos ? 0 : sy, tw, th, pixels);
            return self_;
        }

        bool mipmap = boolAtProp(opts, "mipmap", false);
        bool repeat = boolAtProp(opts, "repeat", false);
        bool clampT = boolAtProp(opts, "clampT", false);
        if (!mn->setCustomShaderTexture(name, tw, th, pixels, mipmap, repeat, clampT, channels))
            return ev::throwTypeError("setShaderTexture: too many sampler uniforms on this node");

        return self_;
    });

    bNode.def("clearShader", 0, [](Value self_, std::span<const Value>) -> Value {
        auto* n = sceneNodeOf(self_);
        if (!isShaderableNode(n))
            return ev::throwTypeError("clearShader: not a MeshNode or InstancedMeshNode");
        if (n->type() == scene::SceneNode::Type::InstancedMesh)
            static_cast<scene::InstancedMeshNode*>(n)->clearCustomShader();
        else
            static_cast<scene::MeshNode*>(n)->clearCustomShader();
        return self_;
    });

    // --- SceneGraph post-processing & culling methods & accessors ---

    bGraph.accessor("renderScale",
        [](Value self_, std::span<const Value>) {
            auto* g = sceneGraphOf(self_);
            return ev::fromDouble(g ? g->renderScale() : 1.0);
        },
        [](Value self_, std::span<const Value> a) {
            auto* g = sceneGraphOf(self_);
            if (g && !a.empty() && ev::isNumber(a[0])) {
                g->setRenderScale(static_cast<float>(ev::toDouble(a[0])));
            }
            return ev::undefined();
        });

    bGraph.def("setRenderScale", 1, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (g && !a.empty() && ev::isNumber(a[0])) {
            g->setRenderScale(static_cast<float>(ev::toDouble(a[0])));
        }
        return self_;
    });

    bGraph.accessor("msaa",
        [](Value self_, std::span<const Value>) {
            auto* g = sceneGraphOf(self_);
            return ev::fromDouble(g ? g->msaa() : 0.0);
        },
        [](Value self_, std::span<const Value> a) {
            auto* g = sceneGraphOf(self_);
            if (g && !a.empty() && ev::isNumber(a[0])) {
                int s = static_cast<int>(ev::toDouble(a[0]));
                g->setMSAA(s);
            }
            return ev::undefined();
        });

    bGraph.def("setMSAA", 1, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (g && !a.empty() && ev::isNumber(a[0])) {
            int s = static_cast<int>(ev::toDouble(a[0]));
            g->setMSAA(s);
        }
        return self_;
    });

    bGraph.def("setColorLUT", 1, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (!g) return ev::fromBool(false);
        if (a.empty() || ev::isNull(a[0]) || ev::isUndefined(a[0])) {
            g->clearColorLUT();
            return ev::fromBool(true);
        }
        if (!ev::isObject(a[0])) return ev::fromBool(false);

        Value pathVal = ev::getProperty(a[0], "path");
        if (!ev::isString(pathVal)) return ev::fromBool(false);
        std::string p = util::resolveAssetPath(ev::toUtf8(pathVal));

        int size = static_cast<int>(numAtProp(a[0], "size", 0));
        float amount = static_cast<float>(numAtProp(a[0], "amount", 1.0));
        return ev::fromBool(g->loadColorLUT(p, size, amount));
    });

    bGraph.def("setFXAA", 1, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (!g) return self_;
        bool enabled = true;
        if (!a.empty()) {
            if (ev::isObject(a[0])) {
                Value enVal = ev::getProperty(a[0], "enabled");
                if (!ev::isUndefined(enVal)) enabled = ev::toBool(enVal);
            } else {
                enabled = ev::toBool(a[0]);
            }
        }
        g->setFXAA(enabled);
        return self_;
    });
}

} // namespace bro::bronze_host

#endif // BRO_WITH_3D
