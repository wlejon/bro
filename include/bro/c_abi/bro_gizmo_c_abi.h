// =============================================================================
// bro_gizmo_c_abi.h — Pure C-ABI declarations for bro.gizmo
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, zero JSContext, direct native CPU register call.
// =============================================================================

#ifndef BRO_GIZMO_C_ABI_H
#define BRO_GIZMO_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Namespace bro.gizmo ---
bool bro_gizmo_get_visible(void);
bool bro_gizmo_get_dragging(void);
const char* bro_gizmo_get_hovered(void);
void bro_gizmo_show(void);
void bro_gizmo_hide(void);
void bro_gizmo_setMode(const char* mode);
void bro_gizmo_setSpace(const char* space);
void bro_gizmo_setPosition(double x, double y, double z);
void bro_gizmo_setOrientation(double x, double y, double z, double w);
void bro_gizmo_configure(void* config);
void bro_gizmo_attach(void* handlers);
void bro_gizmo_detach(void);

#ifdef __cplusplus
}
#endif

#endif // BRO_GIZMO_C_ABI_H
