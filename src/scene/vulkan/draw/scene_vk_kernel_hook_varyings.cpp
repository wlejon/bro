#include "scene/vulkan/draw/scene_vk_kernel_hook.h"

#include <brokernel/ast.h>
#include <brokernel/diagnostics.h>

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

namespace bro::scene::vk {

namespace {

struct VaryingFieldSlot {
    std::string name;
    bool isInt = false;
    bool isUnsigned = false;
    uint32_t lanes = 1;
    uint32_t startSlot = 0;
};

bool classifyVaryingField(const brokernel::StructFieldDecl& f, VaryingFieldSlot& out, std::string& err) {
    out.name = f.name;
    if (!f.type) {
        err = "varying field '" + f.name + "' has no type";
        return false;
    }
    const std::string& tn = f.type->name;
    std::string elem = (!f.type->args.empty() && f.type->args[0].type_arg)
                           ? f.type->args[0].type_arg->name
                           : std::string();

    if (tn == "f32" || tn == "float") {
        out.isInt = false;
        out.lanes = 1;
        return true;
    }
    if (tn == "i32" || tn == "int") {
        out.isInt = true;
        out.isUnsigned = false;
        out.lanes = 1;
        return true;
    }
    if (tn == "u32" || tn == "uint") {
        out.isInt = true;
        out.isUnsigned = true;
        out.lanes = 1;
        return true;
    }
    if (tn == "vec2f" || tn == "vec3f" || tn == "vec4f") {
        out.isInt = false;
        out.lanes = static_cast<uint32_t>(tn[3] - '0');
        return true;
    }
    if (tn == "vec2i" || tn == "vec3i" || tn == "vec4i" ||
        tn == "ivec2" || tn == "ivec3" || tn == "ivec4") {
        out.isInt = true;
        out.isUnsigned = false;
        out.lanes = static_cast<uint32_t>(tn.back() == 'i' ? (tn[3] - '0') : (tn.back() - '0'));
        return true;
    }
    if (tn == "vec2u" || tn == "vec3u" || tn == "vec4u" ||
        tn == "uvec2" || tn == "uvec3" || tn == "uvec4") {
        out.isInt = true;
        out.isUnsigned = true;
        out.lanes = static_cast<uint32_t>(tn.back() == 'u' ? (tn[3] - '0') : (tn.back() - '0'));
        return true;
    }
    if (tn == "vec2" || tn == "vec3" || tn == "vec4") {
        out.lanes = static_cast<uint32_t>(tn[3] - '0');
        if (elem.empty() || elem == "f32" || elem == "float") {
            out.isInt = false;
            return true;
        }
        if (elem == "i32" || elem == "int") {
            out.isInt = true;
            out.isUnsigned = false;
            return true;
        }
        if (elem == "u32" || elem == "uint") {
            out.isInt = true;
            out.isUnsigned = true;
            return true;
        }
    }
    err = "unsupported varying field type '" + tn + "' on field '" + f.name +
          "' (expected f32/vec2..vec4 or i32/u32/ivec2..ivec4)";
    return false;
}

std::string floatSlotExpr(const char* varPrefix, uint32_t slot) {
    static constexpr char kSwiz[4] = {'x', 'y', 'z', 'w'};
    return std::string(varPrefix) + ".v" + std::to_string(slot / 4) + "." + kSwiz[slot % 4];
}

std::string intSlotExpr(const char* varPrefix, uint32_t slot) {
    static constexpr char kSwiz[4] = {'x', 'y', 'z', 'w'};
    return std::string(varPrefix) + ".vi." + kSwiz[slot % 4];
}

struct TextEdit {
    uint32_t start = 0;
    uint32_t end = 0;
    std::string replacement;
};

void collectReturnEdits(const std::vector<brokernel::StmtPtr>& stmts,
                        const std::string& hook,
                        const std::string& vParamName,
                        const std::string& varParamName,
                        std::vector<TextEdit>& edits) {
    for (const auto& sp : stmts) {
        if (!sp) continue;
        const brokernel::Stmt& s = *sp;
        if (s.kind == brokernel::Stmt::Kind::Return) {
            std::string exprText = vParamName;
            if (s.expr && s.expr->span.end.offset > s.expr->span.start.offset &&
                s.expr->span.end.offset <= hook.size()) {
                exprText = hook.substr(s.expr->span.start.offset,
                                       s.expr->span.end.offset - s.expr->span.start.offset);
            }
            edits.push_back({
                s.span.start.offset,
                s.span.end.offset,
                "return __bk_pack_varyings(" + exprText + ", " + varParamName + ");"
            });
        } else {
            collectReturnEdits(s.body, hook, vParamName, varParamName, edits);
            collectReturnEdits(s.else_body, hook, vParamName, varParamName, edits);
        }
    }
}

void collectVoidSurfaceReturnEdits(const std::vector<brokernel::StmtPtr>& stmts,
                                   const std::string& sParamName,
                                   std::vector<TextEdit>& edits) {
    for (const auto& sp : stmts) {
        if (!sp) continue;
        const brokernel::Stmt& s = *sp;
        if (s.kind == brokernel::Stmt::Kind::Return && !s.expr) {
            edits.push_back({
                s.span.start.offset,
                s.span.end.offset,
                "return " + sParamName + ";"
            });
        } else {
            collectVoidSurfaceReturnEdits(s.body, sParamName, edits);
            collectVoidSurfaceReturnEdits(s.else_body, sParamName, edits);
        }
    }
}

uint32_t findBodyOpenBrace(const std::string& hook, const brokernel::FnDecl& fn) {
    uint32_t searchFrom = fn.span.start.offset;
    if (fn.return_type) {
        searchFrom = fn.return_type->span.end.offset;
    } else if (!fn.params.empty()) {
        searchFrom = fn.params.back().span.end.offset;
    }
    for (uint32_t i = searchFrom; i < fn.span.end.offset && i < hook.size(); ++i) {
        if (hook[i] == '{') return i;
    }
    return fn.span.end.offset;
}

} // namespace

bool expandMeshHookVaryings(const std::string& hook, const std::string& file,
                            std::string& outHook, std::string& err) {
    brokernel::DiagnosticBag diags;
    auto ast = brokernel::parse_source(hook, file, diags);
    if (!ast || diags.has_errors()) {
        outHook = hook;
        return true;
    }

    const brokernel::FnDecl* vertFn = nullptr;
    const brokernel::FnDecl* fragFn = nullptr;
    const brokernel::FnDecl* lightFn = nullptr;
    std::string varyingStructName;

    for (const auto& fn : ast->functions) {
        if (fn.name == "vertex") {
            vertFn = &fn;
            if (fn.params.size() == 2 && fn.params[1].type) {
                varyingStructName = fn.params[1].type->name;
            }
        } else if (fn.name == "fragment") {
            fragFn = &fn;
            if (fn.params.size() == 3 && fn.params[1].type && varyingStructName.empty()) {
                varyingStructName = fn.params[1].type->name;
            }
        } else if (fn.name == "light") {
            lightFn = &fn;
            if (fn.params.size() >= 3 && fn.params[1].type && varyingStructName.empty()) {
                varyingStructName = fn.params[1].type->name;
            }
        }
    }

    const bool vertNeedsRewrite = vertFn && (vertFn->params.size() == 2 || !vertFn->return_type);
    const bool fragNeedsRewrite = fragFn && (fragFn->params.size() == 3 || !fragFn->return_type);
    const bool lightNeedsRewrite = lightFn && (lightFn->params.size() >= 3 || !lightFn->return_type);

    if (varyingStructName.empty() && !vertNeedsRewrite && !fragNeedsRewrite && !lightNeedsRewrite) {
        outHook = hook;
        return true;
    }

    std::ostringstream helpers;
    if (!varyingStructName.empty()) {
        const brokernel::StructDecl* vStruct = nullptr;
        for (const auto& st : ast->structs) {
            if (st.name == varyingStructName) {
                vStruct = &st;
                break;
            }
        }
        if (!vStruct) {
            err = "unknown varying struct '" + varyingStructName + "'";
            return false;
        }

        std::vector<VaryingFieldSlot> slots;
        uint32_t floatSlots = 0;
        uint32_t intSlots = 0;
        for (const auto& f : vStruct->fields) {
            VaryingFieldSlot slot;
            if (!classifyVaryingField(f, slot, err)) return false;
            if (slot.isInt) {
                slot.startSlot = intSlots;
                intSlots += slot.lanes;
            } else {
                slot.startSlot = floatSlots;
                floatSlots += slot.lanes;
            }
            slots.push_back(std::move(slot));
        }

        if (floatSlots > 24 || intSlots > 4) {
            err = "varying struct '" + varyingStructName + "' uses " + std::to_string(floatSlots) +
                  " float slots (max 24 across v0..v5) and " + std::to_string(intSlots) +
                  " int slots (max 4 in vi)";
            return false;
        }

        static constexpr char kLaneSwiz[4] = {'x', 'y', 'z', 'w'};
        helpers << "\nfn __bk_pack_varyings(__bk_v_in: mesh::Vertex, __bk_var: " << varyingStructName
                << ") -> mesh::Vertex {\n"
                << "    var __bk_v: mesh::Vertex = __bk_v_in;\n";
        for (const auto& s : slots) {
            for (uint32_t l = 0; l < s.lanes; ++l) {
                std::string rhs = "__bk_var." + s.name;
                if (s.lanes > 1) {
                    rhs += ".";
                    rhs.push_back(kLaneSwiz[l]);
                }
                if (s.isInt) {
                    if (s.isUnsigned) rhs = "(" + rhs + " as i32)";
                    helpers << "    " << intSlotExpr("__bk_v", s.startSlot + l) << " = " << rhs << ";\n";
                } else {
                    helpers << "    " << floatSlotExpr("__bk_v", s.startSlot + l) << " = " << rhs << ";\n";
                }
            }
        }
        helpers << "    return __bk_v;\n}\n";

        helpers << "fn __bk_unpack_varyings(__bk_v: mesh::Vertex) -> " << varyingStructName << " {\n"
                << "    return " << varyingStructName << " {\n";
        for (const auto& s : slots) {
            helpers << "        " << s.name << ": ";
            if (s.lanes == 1) {
                if (s.isInt) {
                    std::string ex = intSlotExpr("__bk_v", s.startSlot);
                    if (s.isUnsigned) ex = "(" + ex + " as u32)";
                    helpers << ex;
                } else {
                    helpers << floatSlotExpr("__bk_v", s.startSlot);
                }
            } else {
                const char* vecType = s.isInt ? (s.isUnsigned ? "u32" : "i32") : "f32";
                helpers << "vec" << s.lanes << "<" << vecType << ">(";
                for (uint32_t l = 0; l < s.lanes; ++l) {
                    if (l > 0) helpers << ", ";
                    if (s.isInt) {
                        std::string ex = intSlotExpr("__bk_v", s.startSlot + l);
                        if (s.isUnsigned) ex = "(" + ex + " as u32)";
                        helpers << ex;
                    } else {
                        helpers << floatSlotExpr("__bk_v", s.startSlot + l);
                    }
                }
                helpers << ")";
            }
            helpers << ",\n";
        }
        helpers << "    };\n}\n";
    }

    std::vector<TextEdit> edits;

    if (vertNeedsRewrite && !vertFn->params.empty()) {
        const std::string vName = vertFn->params[0].name;
        const uint32_t openBrace = findBodyOpenBrace(hook, *vertFn);
        const uint32_t closeBrace = vertFn->span.end.offset > 0 ? vertFn->span.end.offset - 1 : openBrace;
        const char* fnPrefix = vertFn->is_pub ? "fn vertex(" : "pub fn vertex(";
        if (vertFn->params.size() == 2) {
            const std::string varName = vertFn->params[1].name;
            edits.push_back({
                vertFn->span.start.offset,
                openBrace + 1,
                std::string(fnPrefix) + vName + ": mesh::Vertex) -> mesh::Vertex {\n    var " +
                    varName + ": " + varyingStructName + ";"
            });
            collectReturnEdits(vertFn->body, hook, vName, varName, edits);
            const bool endsWithReturn = !vertFn->body.empty() &&
                                        vertFn->body.back()->kind == brokernel::Stmt::Kind::Return;
            if (!endsWithReturn && closeBrace > openBrace) {
                edits.push_back({
                    closeBrace,
                    closeBrace + 1,
                    "\n    return __bk_pack_varyings(" + vName + ", " + varName + ");\n}"
                });
            }
        } else {
            edits.push_back({
                vertFn->span.start.offset,
                openBrace + 1,
                std::string(fnPrefix) + vName + ": mesh::Vertex) -> mesh::Vertex {"
            });
            const bool endsWithReturn = !vertFn->body.empty() &&
                                        vertFn->body.back()->kind == brokernel::Stmt::Kind::Return;
            if (!endsWithReturn && closeBrace > openBrace) {
                edits.push_back({
                    closeBrace,
                    closeBrace + 1,
                    "\n    return " + vName + ";\n}"
                });
            }
        }
    }

    auto rewriteFragOrLight = [&](const brokernel::FnDecl* fn, const char* fnName, const char* userFnName) {
        if (!fn || fn->params.size() < 2) return;
        const uint32_t openBrace = findBodyOpenBrace(hook, *fn);
        const uint32_t closeBrace = fn->span.end.offset > 0 ? fn->span.end.offset - 1 : openBrace;
        if (fn->params.size() >= 3) {
            const std::string vName = fn->params[0].name;
            const std::string varName = fn->params[1].name;
            const std::string sName = fn->params[2].name;
            edits.push_back({
                fn->span.start.offset,
                openBrace + 1,
                std::string("fn ") + userFnName + "(" + vName + ": mesh::Vertex, " +
                    varName + ": " + varyingStructName + ", " + sName + ": mesh::Surface) -> mesh::Surface {"
            });
            collectVoidSurfaceReturnEdits(fn->body, sName, edits);
            const bool endsWithReturn = !fn->body.empty() &&
                                        fn->body.back()->kind == brokernel::Stmt::Kind::Return;
            if (!endsWithReturn && closeBrace > openBrace) {
                edits.push_back({
                    closeBrace,
                    closeBrace + 1,
                    "\n    return " + sName + ";\n}"
                });
            }
            helpers << "pub fn " << fnName << "(__bk_v: mesh::Vertex, __bk_s: mesh::Surface) -> mesh::Surface {\n"
                    << "    return " << userFnName << "(__bk_v, __bk_unpack_varyings(__bk_v), __bk_s);\n"
                    << "}\n";
        } else if (!fn->return_type) {
            const std::string vName = fn->params[0].name;
            const std::string sName = fn->params[1].name;
            const char* fnPrefix = fn->is_pub ? "fn " : "pub fn ";
            edits.push_back({
                fn->span.start.offset,
                openBrace + 1,
                std::string(fnPrefix) + fnName + "(" + vName + ": mesh::Vertex, " +
                    sName + ": mesh::Surface) -> mesh::Surface {"
            });
            collectVoidSurfaceReturnEdits(fn->body, sName, edits);
            const bool endsWithReturn = !fn->body.empty() &&
                                        fn->body.back()->kind == brokernel::Stmt::Kind::Return;
            if (!endsWithReturn && closeBrace > openBrace) {
                edits.push_back({
                    closeBrace,
                    closeBrace + 1,
                    "\n    return " + sName + ";\n}"
                });
            }
        }
    };

    if (fragNeedsRewrite) rewriteFragOrLight(fragFn, "fragment", "__bk_user_fragment");
    if (lightNeedsRewrite) rewriteFragOrLight(lightFn, "light", "__bk_user_light");

    std::sort(edits.begin(), edits.end(), [](const TextEdit& a, const TextEdit& b) {
        return a.start > b.start;
    });

    outHook = hook;
    for (const auto& e : edits) {
        if (e.start <= e.end && e.end <= outHook.size()) {
            outHook.replace(e.start, e.end - e.start, e.replacement);
        }
    }
    outHook += helpers.str();
    return true;
}

} // namespace bro::scene::vk
