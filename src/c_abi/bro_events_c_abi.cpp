// =============================================================================
// bro_events_c_abi.cpp — C++ forwarding implementations for bro.events
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_events_c_abi.h"
#include <cstdint>
#include <string>

struct BroEventImpl {
    std::string type;
    void* target = nullptr;
    void* currentTarget = nullptr;
    bool bubbles = false;
    bool cancelable = false;
    bool defaultPrevented = false;
    double timeStamp = 0.0;
};

struct BroCustomEventImpl {
    BroEventImpl base;
    void* detail = nullptr;
};

struct BroUIEventImpl {
    BroEventImpl base;
    void* view = nullptr;
    int32_t detail = 0;
};

struct BroMouseEventImpl {
    BroUIEventImpl base;
    double screenX = 0;
    double screenY = 0;
    double clientX = 0;
    double clientY = 0;
    double offsetX = 0;
    double offsetY = 0;
    double pageX = 0;
    double pageY = 0;
    bool ctrlKey = false;
    bool shiftKey = false;
    bool altKey = false;
    bool metaKey = false;
    int16_t button = 0;
    uint16_t buttons = 0;
    void* relatedTarget = nullptr;
};

struct BroPointerEventImpl {
    BroMouseEventImpl base;
    int32_t pointerId = 0;
    std::string pointerType = "mouse";
    bool isPrimary = true;
    double pressure = 0.0;
    double width = 1.0;
    double height = 1.0;
};

struct BroTouchImpl {
    int32_t identifier = 0;
    void* target = nullptr;
    double screenX = 0;
    double screenY = 0;
    double clientX = 0;
    double clientY = 0;
    double pageX = 0;
    double pageY = 0;
    double force = 0.0;
};

struct BroTouchListImpl {
    uint32_t length = 0;
};

struct BroTouchEventImpl {
    BroUIEventImpl base;
    void* touches = nullptr;
    void* targetTouches = nullptr;
    void* changedTouches = nullptr;
    bool ctrlKey = false;
    bool shiftKey = false;
    bool altKey = false;
    bool metaKey = false;
};

struct BroGestureEventImpl {
    BroUIEventImpl base;
    double scale = 1.0;
    double rotation = 0.0;
    double clientX = 0;
    double clientY = 0;
};

extern "C" {

// --- Interface bro.events.Event ---
void* bro_Event_create(const char* type, void* /*eventInitDict*/) {
    auto* e = new BroEventImpl();
    if (type) e->type = type;
    return e;
}

void bro_Event_destroy(void* self) {
    delete static_cast<BroEventImpl*>(self);
}

const char* bro_Event_get_type(void* self) {
    return self ? static_cast<BroEventImpl*>(self)->type.c_str() : "";
}

void* bro_Event_get_target(void* self) {
    return self ? static_cast<BroEventImpl*>(self)->target : nullptr;
}

void* bro_Event_get_currentTarget(void* self) {
    return self ? static_cast<BroEventImpl*>(self)->currentTarget : nullptr;
}

bool bro_Event_get_bubbles(void* self) {
    return self ? static_cast<BroEventImpl*>(self)->bubbles : false;
}

bool bro_Event_get_cancelable(void* self) {
    return self ? static_cast<BroEventImpl*>(self)->cancelable : false;
}

bool bro_Event_get_defaultPrevented(void* self) {
    return self ? static_cast<BroEventImpl*>(self)->defaultPrevented : false;
}

double bro_Event_get_timeStamp(void* self) {
    return self ? static_cast<BroEventImpl*>(self)->timeStamp : 0.0;
}

void bro_Event_preventDefault(void* self) {
    if (self) static_cast<BroEventImpl*>(self)->defaultPrevented = true;
}

void bro_Event_stopPropagation(void* /*self*/) {}

void bro_Event_initEvent(void* self, const char* type, bool bubbles, bool cancelable) {
    if (self) {
        auto* e = static_cast<BroEventImpl*>(self);
        if (type) e->type = type;
        e->bubbles = bubbles;
        e->cancelable = cancelable;
        e->defaultPrevented = false;
    }
}

// --- Interface bro.events.CustomEvent ---
void* bro_CustomEvent_create(void) {
    return new BroCustomEventImpl();
}

void bro_CustomEvent_destroy(void* self) {
    delete static_cast<BroCustomEventImpl*>(self);
}

void* bro_CustomEvent_get_detail(void* self) {
    return self ? static_cast<BroCustomEventImpl*>(self)->detail : nullptr;
}

// --- Interface bro.events.UIEvent ---
void* bro_UIEvent_create(void) {
    return new BroUIEventImpl();
}

void bro_UIEvent_destroy(void* self) {
    delete static_cast<BroUIEventImpl*>(self);
}

void* bro_UIEvent_get_view(void* self) {
    return self ? static_cast<BroUIEventImpl*>(self)->view : nullptr;
}

int32_t bro_UIEvent_get_detail(void* self) {
    return self ? static_cast<BroUIEventImpl*>(self)->detail : 0;
}

// --- Interface bro.events.MouseEvent ---
void* bro_MouseEvent_create(void) {
    return new BroMouseEventImpl();
}

void bro_MouseEvent_destroy(void* self) {
    delete static_cast<BroMouseEventImpl*>(self);
}

double bro_MouseEvent_get_screenX(void* self) {
    return self ? static_cast<BroMouseEventImpl*>(self)->screenX : 0.0;
}

double bro_MouseEvent_get_screenY(void* self) {
    return self ? static_cast<BroMouseEventImpl*>(self)->screenY : 0.0;
}

double bro_MouseEvent_get_clientX(void* self) {
    return self ? static_cast<BroMouseEventImpl*>(self)->clientX : 0.0;
}

double bro_MouseEvent_get_clientY(void* self) {
    return self ? static_cast<BroMouseEventImpl*>(self)->clientY : 0.0;
}

double bro_MouseEvent_get_offsetX(void* self) {
    return self ? static_cast<BroMouseEventImpl*>(self)->offsetX : 0.0;
}

double bro_MouseEvent_get_offsetY(void* self) {
    return self ? static_cast<BroMouseEventImpl*>(self)->offsetY : 0.0;
}

double bro_MouseEvent_get_pageX(void* self) {
    return self ? static_cast<BroMouseEventImpl*>(self)->pageX : 0.0;
}

double bro_MouseEvent_get_pageY(void* self) {
    return self ? static_cast<BroMouseEventImpl*>(self)->pageY : 0.0;
}

bool bro_MouseEvent_get_ctrlKey(void* self) {
    return self ? static_cast<BroMouseEventImpl*>(self)->ctrlKey : false;
}

bool bro_MouseEvent_get_shiftKey(void* self) {
    return self ? static_cast<BroMouseEventImpl*>(self)->shiftKey : false;
}

bool bro_MouseEvent_get_altKey(void* self) {
    return self ? static_cast<BroMouseEventImpl*>(self)->altKey : false;
}

bool bro_MouseEvent_get_metaKey(void* self) {
    return self ? static_cast<BroMouseEventImpl*>(self)->metaKey : false;
}

int16_t bro_MouseEvent_get_button(void* self) {
    return self ? static_cast<BroMouseEventImpl*>(self)->button : 0;
}

uint16_t bro_MouseEvent_get_buttons(void* self) {
    return self ? static_cast<BroMouseEventImpl*>(self)->buttons : 0;
}

void* bro_MouseEvent_get_relatedTarget(void* self) {
    return self ? static_cast<BroMouseEventImpl*>(self)->relatedTarget : nullptr;
}

// --- Interface bro.events.PointerEvent ---
void* bro_PointerEvent_create(void) {
    return new BroPointerEventImpl();
}

void bro_PointerEvent_destroy(void* self) {
    delete static_cast<BroPointerEventImpl*>(self);
}

int32_t bro_PointerEvent_get_pointerId(void* self) {
    return self ? static_cast<BroPointerEventImpl*>(self)->pointerId : 0;
}

const char* bro_PointerEvent_get_pointerType(void* self) {
    return self ? static_cast<BroPointerEventImpl*>(self)->pointerType.c_str() : "mouse";
}

bool bro_PointerEvent_get_isPrimary(void* self) {
    return self ? static_cast<BroPointerEventImpl*>(self)->isPrimary : true;
}

double bro_PointerEvent_get_pressure(void* self) {
    return self ? static_cast<BroPointerEventImpl*>(self)->pressure : 0.0;
}

double bro_PointerEvent_get_width(void* self) {
    return self ? static_cast<BroPointerEventImpl*>(self)->width : 1.0;
}

double bro_PointerEvent_get_height(void* self) {
    return self ? static_cast<BroPointerEventImpl*>(self)->height : 1.0;
}

// --- Interface bro.events.Touch ---
void* bro_Touch_create(void) {
    return new BroTouchImpl();
}

void bro_Touch_destroy(void* self) {
    delete static_cast<BroTouchImpl*>(self);
}

int32_t bro_Touch_get_identifier(void* self) {
    return self ? static_cast<BroTouchImpl*>(self)->identifier : 0;
}

void* bro_Touch_get_target(void* self) {
    return self ? static_cast<BroTouchImpl*>(self)->target : nullptr;
}

double bro_Touch_get_screenX(void* self) {
    return self ? static_cast<BroTouchImpl*>(self)->screenX : 0.0;
}

double bro_Touch_get_screenY(void* self) {
    return self ? static_cast<BroTouchImpl*>(self)->screenY : 0.0;
}

double bro_Touch_get_clientX(void* self) {
    return self ? static_cast<BroTouchImpl*>(self)->clientX : 0.0;
}

double bro_Touch_get_clientY(void* self) {
    return self ? static_cast<BroTouchImpl*>(self)->clientY : 0.0;
}

double bro_Touch_get_pageX(void* self) {
    return self ? static_cast<BroTouchImpl*>(self)->pageX : 0.0;
}

double bro_Touch_get_pageY(void* self) {
    return self ? static_cast<BroTouchImpl*>(self)->pageY : 0.0;
}

double bro_Touch_get_force(void* self) {
    return self ? static_cast<BroTouchImpl*>(self)->force : 0.0;
}

// --- Interface bro.events.TouchList ---
void* bro_TouchList_create(void) {
    return new BroTouchListImpl();
}

void bro_TouchList_destroy(void* self) {
    delete static_cast<BroTouchListImpl*>(self);
}

uint32_t bro_TouchList_get_length(void* self) {
    return self ? static_cast<BroTouchListImpl*>(self)->length : 0;
}

void* bro_TouchList_item(void* /*self*/, uint32_t /*index*/) {
    return nullptr;
}

// --- Interface bro.events.TouchEvent ---
void* bro_TouchEvent_create(void) {
    return new BroTouchEventImpl();
}

void bro_TouchEvent_destroy(void* self) {
    delete static_cast<BroTouchEventImpl*>(self);
}

void* bro_TouchEvent_get_touches(void* self) {
    return self ? static_cast<BroTouchEventImpl*>(self)->touches : nullptr;
}

void* bro_TouchEvent_get_targetTouches(void* self) {
    return self ? static_cast<BroTouchEventImpl*>(self)->targetTouches : nullptr;
}

void* bro_TouchEvent_get_changedTouches(void* self) {
    return self ? static_cast<BroTouchEventImpl*>(self)->changedTouches : nullptr;
}

bool bro_TouchEvent_get_ctrlKey(void* self) {
    return self ? static_cast<BroTouchEventImpl*>(self)->ctrlKey : false;
}

bool bro_TouchEvent_get_shiftKey(void* self) {
    return self ? static_cast<BroTouchEventImpl*>(self)->shiftKey : false;
}

bool bro_TouchEvent_get_altKey(void* self) {
    return self ? static_cast<BroTouchEventImpl*>(self)->altKey : false;
}

bool bro_TouchEvent_get_metaKey(void* self) {
    return self ? static_cast<BroTouchEventImpl*>(self)->metaKey : false;
}

// --- Interface bro.events.GestureEvent ---
void* bro_GestureEvent_create(void) {
    return new BroGestureEventImpl();
}

void bro_GestureEvent_destroy(void* self) {
    delete static_cast<BroGestureEventImpl*>(self);
}

double bro_GestureEvent_get_scale(void* self) {
    return self ? static_cast<BroGestureEventImpl*>(self)->scale : 1.0;
}

double bro_GestureEvent_get_rotation(void* self) {
    return self ? static_cast<BroGestureEventImpl*>(self)->rotation : 0.0;
}

double bro_GestureEvent_get_clientX(void* self) {
    return self ? static_cast<BroGestureEventImpl*>(self)->clientX : 0.0;
}

double bro_GestureEvent_get_clientY(void* self) {
    return self ? static_cast<BroGestureEventImpl*>(self)->clientY : 0.0;
}

} // extern "C"
