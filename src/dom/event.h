#pragma once
#include <string>
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
    bool defaultPrevented() const { return defaultPrevented_; }
    double timeStamp() const { return timeStamp_; }

    void setTarget(Element* t) { target_ = t; }
    void setCurrentTarget(Element* t) { currentTarget_ = t; }
    void preventDefault();
    void stopPropagation();
    bool propagationStopped() const { return propagationStopped_; }

private:
    std::string type_;
    Element* target_ = nullptr;
    Element* currentTarget_ = nullptr;
    bool bubbles_;
    bool cancelable_;
    bool defaultPrevented_ = false;
    bool propagationStopped_ = false;
    double timeStamp_;
};

class MouseEvent : public Event {
public:
    MouseEvent(const std::string& type, bool bubbles = true, bool cancelable = true);
    ~MouseEvent() override = default;

    double clientX() const { return clientX_; }
    double clientY() const { return clientY_; }
    int button() const { return button_; }
    int buttons() const { return buttons_; }
    bool ctrlKey() const { return ctrlKey_; }
    bool shiftKey() const { return shiftKey_; }
    bool altKey() const { return altKey_; }

    void setClientX(double v) { clientX_ = v; }
    void setClientY(double v) { clientY_ = v; }
    void setButton(int v) { button_ = v; }
    void setButtons(int v) { buttons_ = v; }
    void setCtrlKey(bool v) { ctrlKey_ = v; }
    void setShiftKey(bool v) { shiftKey_ = v; }
    void setAltKey(bool v) { altKey_ = v; }

private:
    double clientX_ = 0.0;
    double clientY_ = 0.0;
    int button_ = 0;
    int buttons_ = 0;
    bool ctrlKey_ = false;
    bool shiftKey_ = false;
    bool altKey_ = false;
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

    void setKey(const std::string& v) { key_ = v; }
    void setCode(const std::string& v) { code_ = v; }
    void setCtrlKey(bool v) { ctrlKey_ = v; }
    void setShiftKey(bool v) { shiftKey_ = v; }
    void setAltKey(bool v) { altKey_ = v; }
    void setMetaKey(bool v) { metaKey_ = v; }
    void setRepeat(bool v) { repeat_ = v; }

private:
    std::string key_;
    std::string code_;
    bool ctrlKey_ = false;
    bool shiftKey_ = false;
    bool altKey_ = false;
    bool metaKey_ = false;
    bool repeat_ = false;
};

} // namespace bro::dom
