#include "js/dom_bindings_internal.h"

namespace bro::js {

// Notify all registered MutationObservers about a DOM change.
// This checks globalThis.__bro_mutation_observers and calls _notify()
// on each observer whose observed targets match.
void notifyMutationObservers(JSContext* ctx, JSValueConst target,
                             const char* type,
                             const char* attributeName,
                             const char* oldValue,
                             JSValueConst addedNodes,
                             JSValueConst removedNodes)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue observers = JS_GetPropertyStr(ctx, global, "__bro_mutation_observers");
    if (JS_IsUndefined(observers) || JS_IsNull(observers)) {
        JS_FreeValue(ctx, observers);
        JS_FreeValue(ctx, global);
        return;
    }

    JSValue lengthVal = JS_GetPropertyStr(ctx, observers, "length");
    int32_t len = 0;
    JS_ToInt32(ctx, &len, lengthVal);
    JS_FreeValue(ctx, lengthVal);

    if (len <= 0) {
        JS_FreeValue(ctx, observers);
        JS_FreeValue(ctx, global);
        return;
    }

    // Build the MutationRecord object
    JSValue record = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, record, "type", JS_NewString(ctx, type));
    JS_SetPropertyStr(ctx, record, "target", JS_DupValue(ctx, target));
    JS_SetPropertyStr(ctx, record, "addedNodes",
        JS_IsNull(addedNodes) ? JS_NewArray(ctx) : JS_DupValue(ctx, addedNodes));
    JS_SetPropertyStr(ctx, record, "removedNodes",
        JS_IsNull(removedNodes) ? JS_NewArray(ctx) : JS_DupValue(ctx, removedNodes));
    JS_SetPropertyStr(ctx, record, "previousSibling", JS_NULL);
    JS_SetPropertyStr(ctx, record, "nextSibling", JS_NULL);
    JS_SetPropertyStr(ctx, record, "attributeName",
        attributeName ? JS_NewString(ctx, attributeName) : JS_NULL);
    JS_SetPropertyStr(ctx, record, "attributeNamespace", JS_NULL);
    JS_SetPropertyStr(ctx, record, "oldValue",
        oldValue ? JS_NewString(ctx, oldValue) : JS_NULL);

    // Wrap record in an array for _notify()
    JSValue recordsArr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, recordsArr, 0, JS_DupValue(ctx, record));
    JS_FreeValue(ctx, record);

    for (int32_t i = 0; i < len; ++i) {
        JSValue observer = JS_GetPropertyUint32(ctx, observers, static_cast<uint32_t>(i));
        if (JS_IsUndefined(observer) || JS_IsNull(observer)) {
            JS_FreeValue(ctx, observer);
            continue;
        }

        // Check if this observer is watching the target
        JSValue targets = JS_GetPropertyStr(ctx, observer, "_targets");
        if (JS_IsUndefined(targets) || JS_IsNull(targets)) {
            JS_FreeValue(ctx, targets);
            JS_FreeValue(ctx, observer);
            continue;
        }

        JSValue tLenVal = JS_GetPropertyStr(ctx, targets, "length");
        int32_t tLen = 0;
        JS_ToInt32(ctx, &tLen, tLenVal);
        JS_FreeValue(ctx, tLenVal);

        bool matched = false;
        for (int32_t t = 0; t < tLen && !matched; ++t) {
            JSValue entry = JS_GetPropertyUint32(ctx, targets, static_cast<uint32_t>(t));
            JSValue entryTarget = JS_GetPropertyStr(ctx, entry, "target");
            JSValue options = JS_GetPropertyStr(ctx, entry, "options");

            // Check if this entry's target matches (direct match or subtree)
            bool directMatch = false;
            // Compare JS object identity
            if (JS_VALUE_GET_PTR(entryTarget) == JS_VALUE_GET_PTR(target)) {
                directMatch = true;
            }

            bool subtreeMatch = false;
            if (!directMatch) {
                JSValue subtreeVal = JS_GetPropertyStr(ctx, options, "subtree");
                if (JS_ToBool(ctx, subtreeVal)) {
                    // Check if target is a descendant of entryTarget
                    // Walk up the target's parent chain
                    auto* targetNode = unwrapNode(ctx, target);
                    auto* observedNode = unwrapNode(ctx, entryTarget);
                    if (targetNode && observedNode) {
                        for (auto* p = targetNode->parentNode(); p; p = p->parentNode()) {
                            if (p == observedNode) {
                                subtreeMatch = true;
                                break;
                            }
                        }
                    }
                }
                JS_FreeValue(ctx, subtreeVal);
            }

            if (directMatch || subtreeMatch) {
                // Check if the mutation type matches the observer's options
                bool typeMatch = false;
                if (strcmp(type, "childList") == 0) {
                    JSValue childListVal = JS_GetPropertyStr(ctx, options, "childList");
                    typeMatch = JS_ToBool(ctx, childListVal);
                    JS_FreeValue(ctx, childListVal);
                } else if (strcmp(type, "attributes") == 0) {
                    JSValue attrVal = JS_GetPropertyStr(ctx, options, "attributes");
                    typeMatch = JS_ToBool(ctx, attrVal);
                    JS_FreeValue(ctx, attrVal);
                } else if (strcmp(type, "characterData") == 0) {
                    JSValue cdVal = JS_GetPropertyStr(ctx, options, "characterData");
                    typeMatch = JS_ToBool(ctx, cdVal);
                    JS_FreeValue(ctx, cdVal);
                }

                if (typeMatch) matched = true;
            }

            JS_FreeValue(ctx, options);
            JS_FreeValue(ctx, entryTarget);
            JS_FreeValue(ctx, entry);
        }

        if (matched) {
            JSValue notifyFn = JS_GetPropertyStr(ctx, observer, "_notify");
            if (JS_IsFunction(ctx, notifyFn)) {
                JSValue ret = JS_Call(ctx, notifyFn, observer, 1, &recordsArr);
                JS_FreeValue(ctx, ret);
            }
            JS_FreeValue(ctx, notifyFn);
        }

        JS_FreeValue(ctx, targets);
        JS_FreeValue(ctx, observer);
    }

    JS_FreeValue(ctx, recordsArr);
    JS_FreeValue(ctx, observers);
    JS_FreeValue(ctx, global);
}

} // namespace bro::js
