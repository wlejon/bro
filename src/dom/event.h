#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace bro::dom {

class Element;

class Event {
public:
    Event(const std::string& type, bool bubbles = true, bool cancelable = true);
    virtual ~Event() = default;

    const std::string& type() const { return type_; }
    Element* target() const { return target_; }
    Element* currentTarget() const { return currentTarget_; }
    bool bubbles() const { return bubbles_; }
    bool cancelable() const { return cancelable_; }
    bool composed() const { return composed_; }
    bool defaultPrevented() const { return defaultPrevented_; }
    double timeStamp() const { return timeStamp_; }
    bool isTrusted() const { return isTrusted_; }
    int eventPhase() const { return eventPhase_; }

    void setTarget(Element* t) { target_ = t; }
    void setCurrentTarget(Element* t) { currentTarget_ = t; }
    void setComposed(bool v) { composed_ = v; }
    void setIsTrusted(bool v) { isTrusted_ = v; }
    void setEventPhase(int v) { eventPhase_ = v; }
    void preventDefault();
    void stopPropagation();
    void stopImmediatePropagation();
    bool propagationStopped() const { return propagationStopped_; }
    bool immediatePropagationStopped() const { return immediatePropagationStopped_; }

private:
    std::string type_;
    Element* target_ = nullptr;
    Element* currentTarget_ = nullptr;
    bool bubbles_;
    bool cancelable_;
    bool composed_ = false;
    bool defaultPrevented_ = false;
    bool propagationStopped_ = false;
    bool immediatePropagationStopped_ = false;
    bool isTrusted_ = false;
    int eventPhase_ = 0; // 0=NONE, 1=CAPTURING, 2=AT_TARGET, 3=BUBBLING
    double timeStamp_;
};

class MouseEvent : public Event {
public:
    MouseEvent(const std::string& type, bool bubbles = true, bool cancelable = true);
    ~MouseEvent() override = default;

    double clientX() const { return clientX_; }
    double clientY() const { return clientY_; }
    double pageX() const { return pageX_; }
    double pageY() const { return pageY_; }
    double screenX() const { return screenX_; }
    double screenY() const { return screenY_; }
    double offsetX() const { return offsetX_; }
    double offsetY() const { return offsetY_; }
    double movementX() const { return movementX_; }
    double movementY() const { return movementY_; }
    int button() const { return button_; }
    int buttons() const { return buttons_; }
    int detail() const { return detail_; }
    bool ctrlKey() const { return ctrlKey_; }
    bool shiftKey() const { return shiftKey_; }
    bool altKey() const { return altKey_; }
    bool metaKey() const { return metaKey_; }
    Element* relatedTarget() const { return relatedTarget_; }

    void setClientX(double v) { clientX_ = v; }
    void setClientY(double v) { clientY_ = v; }
    void setPageX(double v) { pageX_ = v; }
    void setPageY(double v) { pageY_ = v; }
    void setScreenX(double v) { screenX_ = v; }
    void setScreenY(double v) { screenY_ = v; }
    void setOffsetX(double v) { offsetX_ = v; }
    void setOffsetY(double v) { offsetY_ = v; }
    void setMovementX(double v) { movementX_ = v; }
    void setMovementY(double v) { movementY_ = v; }
    void setButton(int v) { button_ = v; }
    void setButtons(int v) { buttons_ = v; }
    void setDetail(int v) { detail_ = v; }
    void setCtrlKey(bool v) { ctrlKey_ = v; }
    void setShiftKey(bool v) { shiftKey_ = v; }
    void setAltKey(bool v) { altKey_ = v; }
    void setMetaKey(bool v) { metaKey_ = v; }
    void setRelatedTarget(Element* v) { relatedTarget_ = v; }

private:
    double clientX_ = 0.0;
    double clientY_ = 0.0;
    double pageX_ = 0.0;
    double pageY_ = 0.0;
    double screenX_ = 0.0;
    double screenY_ = 0.0;
    double offsetX_ = 0.0;
    double offsetY_ = 0.0;
    double movementX_ = 0.0;
    double movementY_ = 0.0;
    int button_ = 0;
    int buttons_ = 0;
    int detail_ = 0;
    bool ctrlKey_ = false;
    bool shiftKey_ = false;
    bool altKey_ = false;
    bool metaKey_ = false;
    Element* relatedTarget_ = nullptr;
};

class KeyboardEvent : public Event {
public:
    KeyboardEvent(const std::string& type, bool bubbles = true, bool cancelable = true);
    ~KeyboardEvent() override = default;

    const std::string& key() const { return key_; }
    const std::string& code() const { return code_; }
    bool ctrlKey() const { return ctrlKey_; }
    bool shiftKey() const { return shiftKey_; }
    bool altKey() const { return altKey_; }
    bool metaKey() const { return metaKey_; }
    bool repeat() const { return repeat_; }
    bool isComposing() const { return isComposing_; }
    int location() const { return location_; }

    void setKey(const std::string& v) { key_ = v; }
    void setCode(const std::string& v) { code_ = v; }
    void setCtrlKey(bool v) { ctrlKey_ = v; }
    void setShiftKey(bool v) { shiftKey_ = v; }
    void setAltKey(bool v) { altKey_ = v; }
    void setMetaKey(bool v) { metaKey_ = v; }
    void setRepeat(bool v) { repeat_ = v; }
    void setIsComposing(bool v) { isComposing_ = v; }
    void setLocation(int v) { location_ = v; }

private:
    std::string key_;
    std::string code_;
    bool ctrlKey_ = false;
    bool shiftKey_ = false;
    bool altKey_ = false;
    bool metaKey_ = false;
    bool repeat_ = false;
    bool isComposing_ = false;
    int location_ = 0; // 0=STANDARD, 1=LEFT, 2=RIGHT, 3=NUMPAD
};

class FocusEvent : public Event {
public:
    FocusEvent(const std::string& type, bool bubbles = false, bool cancelable = false);
    ~FocusEvent() override = default;

    Element* relatedTarget() const { return relatedTarget_; }
    void setRelatedTarget(Element* v) { relatedTarget_ = v; }

private:
    Element* relatedTarget_ = nullptr;
};

class WheelEvent : public MouseEvent {
public:
    WheelEvent(const std::string& type, bool bubbles = true, bool cancelable = true);
    ~WheelEvent() override = default;

    static constexpr int DOM_DELTA_PIXEL = 0;
    static constexpr int DOM_DELTA_LINE = 1;
    static constexpr int DOM_DELTA_PAGE = 2;

    double deltaX() const { return deltaX_; }
    double deltaY() const { return deltaY_; }
    double deltaZ() const { return deltaZ_; }
    int deltaMode() const { return deltaMode_; }

    void setDeltaX(double v) { deltaX_ = v; }
    void setDeltaY(double v) { deltaY_ = v; }
    void setDeltaZ(double v) { deltaZ_ = v; }
    void setDeltaMode(int v) { deltaMode_ = v; }

private:
    double deltaX_ = 0.0;
    double deltaY_ = 0.0;
    double deltaZ_ = 0.0;
    int deltaMode_ = 0; // DOM_DELTA_PIXEL
};

class InputEvent : public Event {
public:
    InputEvent(const std::string& type, bool bubbles = true, bool cancelable = false);
    ~InputEvent() override = default;

    const std::string& data() const { return data_; }
    const std::string& inputType() const { return inputType_; }
    bool isComposing() const { return isComposing_; }

    void setData(const std::string& v) { data_ = v; }
    void setInputType(const std::string& v) { inputType_ = v; }
    void setIsComposing(bool v) { isComposing_ = v; }

private:
    std::string data_;
    std::string inputType_;
    bool isComposing_ = false;
};

class TransitionEvent : public Event {
public:
    TransitionEvent(const std::string& type, bool bubbles = true, bool cancelable = false);
    ~TransitionEvent() override = default;

    const std::string& propertyName() const { return propertyName_; }
    double elapsedTime() const { return elapsedTime_; }
    const std::string& pseudoElement() const { return pseudoElement_; }

    void setPropertyName(const std::string& v) { propertyName_ = v; }
    void setElapsedTime(double v) { elapsedTime_ = v; }
    void setPseudoElement(const std::string& v) { pseudoElement_ = v; }

private:
    std::string propertyName_;
    double elapsedTime_ = 0.0;
    std::string pseudoElement_;
};

class AnimationEvent : public Event {
public:
    AnimationEvent(const std::string& type, bool bubbles = true, bool cancelable = false);
    ~AnimationEvent() override = default;

    const std::string& animationName() const { return animationName_; }
    double elapsedTime() const { return elapsedTime_; }
    const std::string& pseudoElement() const { return pseudoElement_; }

    void setAnimationName(const std::string& v) { animationName_ = v; }
    void setElapsedTime(double v) { elapsedTime_ = v; }
    void setPseudoElement(const std::string& v) { pseudoElement_ = v; }

private:
    std::string animationName_;
    double elapsedTime_ = 0.0;
    std::string pseudoElement_;
};

// A single entry on ClipboardEvent.clipboardData. Bytes are populated for
// binary items (image/png, image/bmp, …); text is populated for string items.
struct ClipboardItem {
    std::string mime;
    std::vector<uint8_t> bytes;
    std::string text;
};

class ClipboardEvent : public Event {
public:
    ClipboardEvent(const std::string& type, bool bubbles = true, bool cancelable = true);
    ~ClipboardEvent() override = default;

    const std::string& clipboardText() const { return clipboardText_; }
    void setClipboardText(const std::string& v) { clipboardText_ = v; }

    const std::vector<ClipboardItem>& items() const { return items_; }
    void addItem(ClipboardItem item) { items_.push_back(std::move(item)); }

private:
    std::string clipboardText_;
    std::vector<ClipboardItem> items_;
};

// HTML SubmitEvent: fired on <form> when the form is submitted interactively
// (not via the .submit() JS method). Carries the submit button that
// triggered the submission, or null if submission wasn't button-triggered.
// Element* is owned by the DOM; the event only holds a non-owning pointer.
class SubmitEvent : public Event {
public:
    SubmitEvent(const std::string& type, bool bubbles = true, bool cancelable = true);
    ~SubmitEvent() override = default;

    Element* submitter() const { return submitter_; }
    void setSubmitter(Element* e) { submitter_ = e; }

private:
    Element* submitter_ = nullptr;
};

class DragEvent : public MouseEvent {
public:
    DragEvent(const std::string& type, bool bubbles = true, bool cancelable = true);
    ~DragEvent() override = default;

    // dataTransfer.files paths (for file drops)
    const std::vector<std::string>& files() const { return files_; }
    void addFile(const std::string& path) { files_.push_back(path); }

    // dataTransfer text data
    const std::string& dataText() const { return dataText_; }
    void setDataText(const std::string& v) { dataText_ = v; }

private:
    std::vector<std::string> files_;
    std::string dataText_;
};

} // namespace bro::dom
