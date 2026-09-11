// =============================================================================
// bro_window_c_abi.h — Pure C-ABI declarations for bro.window
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, direct native CPU register call.
// =============================================================================

#ifndef BRO_WINDOW_C_ABI_H
#define BRO_WINDOW_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Namespace bro.window ---
const char* bro_window_get_state(void);
bool bro_window_get_borderless(void);
void bro_window_set_borderless(bool val);
bool bro_window_get_alwaysOnTop(void);
void bro_window_set_alwaysOnTop(bool val);
void bro_window_minimize(void);
void bro_window_maximize(void);
void bro_window_restore(void);
void* bro_window_getPosition(void);
int32_t bro_window_getPositionX(void);
int32_t bro_window_getPositionY(void);
void bro_window_setPosition(int32_t x, int32_t y);
void* bro_window_getMinSize(void);
int32_t bro_window_getMinWidth(void);
int32_t bro_window_getMinHeight(void);
void bro_window_setMinSize(int32_t width, int32_t height);
void* bro_window_getMaxSize(void);
int32_t bro_window_getMaxWidth(void);
int32_t bro_window_getMaxHeight(void);
void bro_window_setMaxSize(int32_t width, int32_t height);
void* bro_window_getDisplays(void);
int32_t bro_window_getDisplayCount(void);
bool bro_window_moveToDisplay(uint32_t id);

#ifdef __cplusplus
}
#endif

#endif // BRO_WINDOW_C_ABI_H
