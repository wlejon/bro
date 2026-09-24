// HTMLMediaElement and HTMLVideoElement: the playback surface over
// layout::ElVideo (src, load/play/pause, currentTime, duration, readyState,
// networkState, volume, rate, loop, the TimeRanges) and the video-only half
// (videoWidth/Height/Rotation, frameRate, stepFrame). The control is created
// lazily on first `src` / `load()`, the way the parser creates it for a
// <video src> in markup. Decorates the classes host_html_interfaces.cpp
// installs; docs/video-api.js is the contract.

#include "bronze_host/host_element_video.h"
#include "bronze_host/host_internal.h"
#include "bronze_host/gl_internal.h"

#include "dom/element.h"
#include "dom/event.h"
#include "dom/event_dispatch.h"
#include "engine/engine.h"
#include "layout/el_video.h"

#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>

namespace bro::bronze_host {

namespace {

static dom::Element* getElement(Value self) {
    HostNodeState* st = hostNodeStateOfValue(self);
    return st ? st->el : nullptr;
}

static layout::ElVideo* getVideoControl(dom::Element* el, bool createIfMissing = false) {
    if (!el) return nullptr;
    if (auto* v = el->videoControl()) return v;
    if (createIfMissing && (el->tagName() == "VIDEO" || el->tagName() == "video" ||
                            el->tagName() == "AUDIO" || el->tagName() == "audio")) {
        if (auto* eng = hostEngine()) {
            auto ctrl = std::make_unique<layout::ElVideo>(eng->renderer());
            ctrl->setElement(el);
            ctrl->setAudioEngine(eng->audioEngine());
            el->setVideoControl(std::move(ctrl));
            return el->videoControl();
        }
    }
    return nullptr;
}

static void fireMediaEvent(dom::Element* el, const char* type) {
    if (!el) return;
    dom::Event evt(type, false, false);
    evt.setIsTrusted(true);
    dom::dispatchDomEvent(el, evt);
}

static Value buildTimeRanges(double duration, bool present) {
    ObjectBuilder obj;
    int32_t length = present ? 1 : 0;
    obj.set("length", ev::fromDouble(length));
    obj.def("start", 1, [length](Value, std::span<const Value> a) -> Value {
        if (a.empty() || !ev::isNumber(a[0])) {
            return ev::throwValue(hostMakeDomError("IndexSizeError", "Index is out of range."));
        }
        double idx = ev::toDouble(a[0]);
        if (idx < 0 || idx >= length || std::isnan(idx)) {
            return ev::throwValue(hostMakeDomError("IndexSizeError", "Index is out of range."));
        }
        return ev::fromDouble(0.0);
    });
    obj.def("end", 1, [duration, length](Value, std::span<const Value> a) -> Value {
        if (a.empty() || !ev::isNumber(a[0])) {
            return ev::throwValue(hostMakeDomError("IndexSizeError", "Index is out of range."));
        }
        double idx = ev::toDouble(a[0]);
        if (idx < 0 || idx >= length || std::isnan(idx)) {
            return ev::throwValue(hostMakeDomError("IndexSizeError", "Index is out of range."));
        }
        return ev::fromDouble(duration);
    });
    return obj.get();
}

} // namespace

void decorateMediaProto(ObjectBuilder& b) {
    // Methods
    b.def("play", 0, [](Value self, std::span<const Value>) -> Value {
        auto* el = getElement(self);
        // Rooted: making the DOMException, and the 'play' event, allocate.
        ev::Persistent p(ev::createPromise());
        auto reject = [&p](const char* msg) {
            ev::Persistent err(hostMakeDomError("NotSupportedError", msg));
            ev::rejectPromise(p.get(), err.get());
            return p.get();
        };
        if (!el) return reject("The element has no supported sources.");
        std::string src = el->getAttribute("src");
        if (src.empty()) {
            for (auto* kid : el->childNodes()) {
                if (auto* kel = dynamic_cast<dom::Element*>(kid)) {
                    if (kel->tagName() == "SOURCE" || kel->tagName() == "source") {
                        std::string ksrc = kel->getAttribute("src");
                        if (!ksrc.empty()) {
                            src = ksrc;
                            break;
                        }
                    }
                }
            }
        }
        if (src.empty()) return reject("The element has no supported sources.");
        auto* v = getVideoControl(el, true);
        if (!v) return reject("Failed to create media pipeline.");
        // A source the last load could not open stays unplayable until the
        // next load; retrying it here would fire a second `error`.
        if (!v->hasPipeline() && v->errorCode() != 0)
            return reject("The element has no supported sources.");
        if (!v->hasPipeline()) {
            if (!v->load(src)) return reject("The media resource failed to load.");
        }
        bool wasPaused = !v->isPlaying();
        v->play();
        if (wasPaused) fireMediaEvent(el, "play");
        ev::resolvePromise(p.get(), ev::undefined());
        return p.get();
    });

    b.def("pause", 0, [](Value self, std::span<const Value>) -> Value {
        auto* el = getElement(self);
        if (el) {
            if (auto* v = el->videoControl()) {
                bool wasPlaying = v->isPlaying();
                v->pause();
                if (wasPlaying) fireMediaEvent(el, "pause");
            }
        }
        return ev::undefined();
    });

    b.def("load", 0, [](Value self, std::span<const Value>) -> Value {
        auto* el = getElement(self);
        if (!el) return ev::undefined();
        auto* v = getVideoControl(el, true);
        if (!v) return ev::undefined();
        std::string src = el->getAttribute("src");
        // Always runs the load algorithm: with no src that is just the reset
        // (and `emptied` if something was loaded).
        if (!src.empty()) v->load(src);
        else v->unload();
        return ev::undefined();
    });

    b.def("canPlayType", 1, [](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::fromUtf8("");
        std::string mime = ev::toUtf8(a[0]);
        auto startsWith = [&](const char* p) {
            size_t n = strlen(p);
            return mime.size() >= n &&
                   std::equal(mime.begin(), mime.begin() + n, p,
                              [](char c1, char c2){ return std::tolower((unsigned char)c1) == c2; });
        };
        if (startsWith("video/webm") || startsWith("audio/webm") ||
            startsWith("audio/ogg") || mime.find("opus") != std::string::npos ||
            mime.find("vp9") != std::string::npos || mime.find("vp8") != std::string::npos) {
            return ev::fromUtf8("probably");
        }
        return ev::fromUtf8("");
    });

    // Accessors
    b.accessor("src",
        [](Value self, std::span<const Value>) -> Value {
            auto* el = getElement(self);
            if (!el) return ev::fromUtf8("");
            return ev::fromUtf8(el->getAttribute("src"));
        },
        [](Value self, std::span<const Value> a) -> Value {
            auto* el = getElement(self);
            if (!el) return ev::undefined();
            std::string s = a.empty() || ev::isUndefined(a[0]) ? "" : ev::toUtf8(a[0]);
            // Setting src always runs the load algorithm. Once the element has
            // a control, setAttribute("src") starts it for a changed value;
            // a control made here, or the same value again, needs telling.
            const bool hadControl = el->videoControl() != nullptr;
            const bool same = el->hasAttribute("src") && el->getAttribute("src") == s;
            el->setAttribute("src", s);
            auto* v = getVideoControl(el, true);
            if (v && !s.empty() && (!hadControl || same)) v->load(s);
            return ev::undefined();
        });

    // HTMLMediaElement.error: null, or a MediaError-shaped object for the
    // source the last load could not open.
    b.accessor("error",
        [](Value self, std::span<const Value>) -> Value {
            auto* el = getElement(self);
            auto* v = el ? el->videoControl() : nullptr;
            if (!v || v->errorCode() == 0) return ev::null();
            ObjectBuilder err;
            err.set("code", ev::fromDouble(v->errorCode()));
            err.set("message", ev::fromUtf8(v->errorMessage()));
            err.set("MEDIA_ERR_ABORTED", ev::fromDouble(1));
            err.set("MEDIA_ERR_NETWORK", ev::fromDouble(2));
            err.set("MEDIA_ERR_DECODE", ev::fromDouble(3));
            err.set("MEDIA_ERR_SRC_NOT_SUPPORTED", ev::fromDouble(4));
            return err.get();
        }, nullptr);

    b.accessor("currentSrc",
        [](Value self, std::span<const Value>) -> Value {
            auto* el = getElement(self);
            if (!el) return ev::fromUtf8("");
            if (auto* v = el->videoControl()) {
                return ev::fromUtf8(v->currentSrc());
            }
            return ev::fromUtf8("");
        }, nullptr);

    b.accessor("currentTime",
        [](Value self, std::span<const Value>) -> Value {
            auto* el = getElement(self);
            double t = 0.0;
            if (el) {
                if (auto* v = el->videoControl()) t = v->currentTime();
            }
            return ev::fromDouble(t);
        },
        [](Value self, std::span<const Value> a) -> Value {
            auto* el = getElement(self);
            if (!el || a.empty()) return ev::undefined();
            double t = ev::toDouble(a[0]);
            if (auto* v = el->videoControl()) {
                fireMediaEvent(el, "seeking");
                v->seekTo(t);
                fireMediaEvent(el, "seeked");
                fireMediaEvent(el, "timeupdate");
            }
            return ev::undefined();
        });

    b.accessor("duration",
        [](Value self, std::span<const Value>) -> Value {
            auto* el = getElement(self);
            if (!el) return ev::fromDouble(std::numeric_limits<double>::quiet_NaN());
            auto* v = el->videoControl();
            if (!v || !v->hasPipeline()) return ev::fromDouble(std::numeric_limits<double>::quiet_NaN());
            return ev::fromDouble(v->duration());
        }, nullptr);

    b.accessor("paused",
        [](Value self, std::span<const Value>) -> Value {
            auto* el = getElement(self);
            if (!el) return ev::fromBool(true);
            if (auto* v = el->videoControl()) return ev::fromBool(!v->isPlaying());
            return ev::fromBool(true);
        }, nullptr);

    b.accessor("ended",
        [](Value self, std::span<const Value>) -> Value {
            auto* el = getElement(self);
            if (!el) return ev::fromBool(false);
            if (auto* v = el->videoControl()) return ev::fromBool(v->isEnded());
            return ev::fromBool(false);
        }, nullptr);

    b.accessor("seeking",
        [](Value self, std::span<const Value>) -> Value {
            auto* el = getElement(self);
            if (!el) return ev::fromBool(false);
            if (auto* v = el->videoControl()) return ev::fromBool(v->isSeeking());
            return ev::fromBool(false);
        }, nullptr);

    b.accessor("readyState",
        [](Value self, std::span<const Value>) -> Value {
            auto* el = getElement(self);
            if (!el) return ev::fromDouble(0);
            auto* v = el->videoControl();
            if (!v || !v->hasPipeline()) return ev::fromDouble(0);
            if (v->isReady()) return ev::fromDouble(4);
            return ev::fromDouble(1);
        }, nullptr);

    b.accessor("networkState",
        [](Value self, std::span<const Value>) -> Value {
            auto* el = getElement(self);
            if (!el) return ev::fromDouble(0);
            std::string src = el->getAttribute("src");
            if (src.empty()) return ev::fromDouble(0);
            auto* v = el->videoControl();
            if (v && v->hasPipeline()) return ev::fromDouble(1);
            return ev::fromDouble(3);
        }, nullptr);

    b.accessor("volume",
        [](Value self, std::span<const Value>) -> Value {
            auto* el = getElement(self);
            if (!el) return ev::fromDouble(1.0);
            if (auto* v = el->videoControl()) return ev::fromDouble(v->volume());
            return ev::fromDouble(1.0);
        },
        [](Value self, std::span<const Value> a) -> Value {
            auto* el = getElement(self);
            if (!el || a.empty()) return ev::undefined();
            auto* v = el->videoControl();
            if (!v) return ev::undefined();
            double d = ev::toDouble(a[0]);
            double prev = v->volume();
            v->setVolume(d);
            if (v->volume() != prev) fireMediaEvent(el, "volumechange");
            return ev::undefined();
        });

    b.accessor("muted",
        [](Value self, std::span<const Value>) -> Value {
            auto* el = getElement(self);
            if (!el) return ev::fromBool(false);
            if (auto* v = el->videoControl()) return ev::fromBool(v->muted());
            return ev::fromBool(el->hasAttribute("muted"));
        },
        [](Value self, std::span<const Value> a) -> Value {
            auto* el = getElement(self);
            if (!el || a.empty()) return ev::undefined();
            bool b = ev::toBool(a[0]);
            if (b) el->setAttribute("muted", "");
            else el->removeAttribute("muted");
            auto* v = el->videoControl();
            if (!v) return ev::undefined();
            bool prev = v->muted();
            v->setMuted(b);
            if (v->muted() != prev) fireMediaEvent(el, "volumechange");
            return ev::undefined();
        });

    b.accessor("defaultMuted",
        [](Value self, std::span<const Value>) -> Value {
            auto* el = getElement(self);
            return ev::fromBool(el && el->hasAttribute("muted"));
        },
        [](Value self, std::span<const Value> a) -> Value {
            auto* el = getElement(self);
            if (!el || a.empty()) return ev::undefined();
            if (ev::toBool(a[0])) el->setAttribute("muted", "");
            else el->removeAttribute("muted");
            return ev::undefined();
        });

    b.accessor("playbackRate",
        [](Value self, std::span<const Value>) -> Value {
            auto* el = getElement(self);
            if (!el) return ev::fromDouble(1.0);
            if (auto* v = el->videoControl()) return ev::fromDouble(v->playbackRate());
            return ev::fromDouble(1.0);
        },
        [](Value self, std::span<const Value> a) -> Value {
            auto* el = getElement(self);
            if (!el || a.empty()) return ev::undefined();
            auto* v = el->videoControl();
            if (!v) return ev::undefined();
            double d = ev::toDouble(a[0]);
            double prev = v->playbackRate();
            v->setPlaybackRate(d);
            if (v->playbackRate() != prev) fireMediaEvent(el, "ratechange");
            return ev::undefined();
        });

    b.accessor("defaultPlaybackRate",
        [](Value self, std::span<const Value>) -> Value {
            auto* el = getElement(self);
            if (!el) return ev::fromDouble(1.0);
            if (auto* v = el->videoControl()) return ev::fromDouble(v->defaultPlaybackRate());
            return ev::fromDouble(1.0);
        },
        [](Value self, std::span<const Value> a) -> Value {
            auto* el = getElement(self);
            if (!el || a.empty()) return ev::undefined();
            if (auto* v = el->videoControl()) v->setDefaultPlaybackRate(ev::toDouble(a[0]));
            return ev::undefined();
        });

    b.accessor("loop",
        [](Value self, std::span<const Value>) -> Value {
            auto* el = getElement(self);
            return ev::fromBool(el && el->hasAttribute("loop"));
        },
        [](Value self, std::span<const Value> a) -> Value {
            auto* el = getElement(self);
            if (!el || a.empty()) return ev::undefined();
            bool b = ev::toBool(a[0]);
            if (b) el->setAttribute("loop", "");
            else el->removeAttribute("loop");
            if (auto* v = el->videoControl()) v->setLoopEnabled(b);
            return ev::undefined();
        });

    b.accessor("autoplay",
        [](Value self, std::span<const Value>) -> Value {
            auto* el = getElement(self);
            return ev::fromBool(el && el->hasAttribute("autoplay"));
        },
        [](Value self, std::span<const Value> a) -> Value {
            auto* el = getElement(self);
            if (!el || a.empty()) return ev::undefined();
            if (ev::toBool(a[0])) el->setAttribute("autoplay", "");
            else el->removeAttribute("autoplay");
            return ev::undefined();
        });

    b.accessor("controls",
        [](Value self, std::span<const Value>) -> Value {
            auto* el = getElement(self);
            return ev::fromBool(el && el->hasAttribute("controls"));
        },
        [](Value self, std::span<const Value> a) -> Value {
            auto* el = getElement(self);
            if (!el || a.empty()) return ev::undefined();
            if (ev::toBool(a[0])) el->setAttribute("controls", "");
            else el->removeAttribute("controls");
            return ev::undefined();
        });

    b.accessor("preload",
        [](Value self, std::span<const Value>) -> Value {
            auto* el = getElement(self);
            if (!el) return ev::fromUtf8("metadata");
            std::string p = el->getAttribute("preload");
            return ev::fromUtf8(p.empty() ? "metadata" : p);
        },
        [](Value self, std::span<const Value> a) -> Value {
            auto* el = getElement(self);
            if (!el || a.empty()) return ev::undefined();
            el->setAttribute("preload", ev::toUtf8(a[0]));
            return ev::undefined();
        });

    b.accessor("buffered",
        [](Value self, std::span<const Value>) -> Value {
            auto* el = getElement(self);
            if (!el) return buildTimeRanges(0.0, false);
            auto* v = el->videoControl();
            bool present = v && v->hasPipeline();
            return buildTimeRanges(v ? v->duration() : 0.0, present);
        }, nullptr);

    b.accessor("seekable",
        [](Value self, std::span<const Value>) -> Value {
            auto* el = getElement(self);
            if (!el) return buildTimeRanges(0.0, false);
            auto* v = el->videoControl();
            bool present = v && v->hasPipeline();
            return buildTimeRanges(v ? v->duration() : 0.0, present);
        }, nullptr);

    b.accessor("played",
        [](Value self, std::span<const Value>) -> Value {
            auto* el = getElement(self);
            if (!el) return buildTimeRanges(0.0, false);
            auto* v = el->videoControl();
            if (!v || !v->hasPipeline()) return buildTimeRanges(0.0, false);
            return buildTimeRanges(v->currentTime(), v->currentTime() > 0.0);
        }, nullptr);
}

void decorateVideoProto(ObjectBuilder& b) {
    b.accessor("videoWidth",
        [](Value self, std::span<const Value>) -> Value {
            auto* el = getElement(self);
            if (el) {
                if (auto* v = el->videoControl()) return ev::fromDouble(v->videoWidth());
            }
            return ev::fromDouble(0);
        }, nullptr);

    b.accessor("videoHeight",
        [](Value self, std::span<const Value>) -> Value {
            auto* el = getElement(self);
            if (el) {
                if (auto* v = el->videoControl()) return ev::fromDouble(v->videoHeight());
            }
            return ev::fromDouble(0);
        }, nullptr);

    b.accessor("videoRotation",
        [](Value self, std::span<const Value>) -> Value {
            auto* el = getElement(self);
            if (el) {
                if (auto* v = el->videoControl()) return ev::fromDouble(v->videoRotation());
            }
            return ev::fromDouble(0);
        }, nullptr);

    b.accessor("frameRate",
        [](Value self, std::span<const Value>) -> Value {
            auto* el = getElement(self);
            double r = 0.0;
            if (el) {
                if (auto* v = el->videoControl()) r = v->frameRate();
            }
            return ev::fromDouble(r);
        }, nullptr);

    b.accessor("width",
        [](Value self, std::span<const Value>) -> Value {
            auto* el = getElement(self);
            if (!el || !el->hasAttribute("width")) return ev::fromDouble(0);
            return ev::fromDouble(std::atoi(el->getAttribute("width").c_str()));
        },
        [](Value self, std::span<const Value> a) -> Value {
            auto* el = getElement(self);
            if (el && !a.empty()) {
                el->setAttribute("width", std::to_string(satCast<int>(ev::toDouble(a[0]))));
            }
            return ev::undefined();
        });

    b.accessor("height",
        [](Value self, std::span<const Value>) -> Value {
            auto* el = getElement(self);
            if (!el || !el->hasAttribute("height")) return ev::fromDouble(0);
            return ev::fromDouble(std::atoi(el->getAttribute("height").c_str()));
        },
        [](Value self, std::span<const Value> a) -> Value {
            auto* el = getElement(self);
            if (el && !a.empty()) {
                el->setAttribute("height", std::to_string(satCast<int>(ev::toDouble(a[0]))));
            }
            return ev::undefined();
        });

    b.accessor("poster",
        [](Value self, std::span<const Value>) -> Value {
            auto* el = getElement(self);
            return el ? ev::fromUtf8(el->getAttribute("poster")) : ev::fromUtf8("");
        },
        [](Value self, std::span<const Value> a) -> Value {
            auto* el = getElement(self);
            if (el && !a.empty()) el->setAttribute("poster", ev::toUtf8(a[0]));
            return ev::undefined();
        });

    b.def("stepFrame", 1, [](Value self, std::span<const Value> a) -> Value {
        auto* el = getElement(self);
        if (!el) return ev::fromDouble(0);
        auto* v = el->videoControl();
        if (!v) return ev::fromDouble(0);

        int32_t frames = 1;
        if (!a.empty() && !ev::isUndefined(a[0])) {
            frames = satCast<int32_t>(ev::toDouble(a[0]));
        }
        fireMediaEvent(el, "seeking");
        const int moved = v->stepFrame(frames);
        fireMediaEvent(el, "seeked");
        if (moved) fireMediaEvent(el, "timeupdate");
        return ev::fromDouble(moved);
    });
}

}  // namespace bro::bronze_host
