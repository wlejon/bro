// Web Audio & Sound Engine Integration — Spatial Audio Subsystem
//
// Full implementation of PannerNode, StereoPannerNode, and spatial calculations.

#include "bronze_host/host_audio_internal.h"

namespace bro::bronze_host {

// ---------------------------------------------------------------------------
// Destructors (Finalizers)
// ---------------------------------------------------------------------------

void hostPannerDtor(void* p) {
    delete static_cast<HostPannerNode*>(p);
}

void hostStereoPannerDtor(void* p) {
    delete static_cast<HostStereoPannerNode*>(p);
}

// ---------------------------------------------------------------------------
// PannerNode
// ---------------------------------------------------------------------------

void decoratePannerNodeProto(ObjectBuilder& b) {
    b.accessor("panningModel",
               [](Value self_, std::span<const Value>) {
                   HostPannerNode* p = pannerOf(self_);
                   if (!p) return ev::undefined();
                   return ev::fromUtf8(p->panningModel);
               },
               [](Value self_, std::span<const Value> a) {
                   HostPannerNode* p = pannerOf(self_);
                   if (!p || a.empty() || ev::isObject(a[0]) || ev::isUndefined(a[0])) return ev::undefined();
                   std::string m = ev::toUtf8(a[0]);
                   if (m == "equalpower" || m == "HRTF") {
                       p->panningModel = m;
                   }
                   return ev::undefined();
               });

    b.accessor("distanceModel",
               [](Value self_, std::span<const Value>) {
                   HostPannerNode* p = pannerOf(self_);
                   if (!p) return ev::undefined();
                   return ev::fromUtf8(p->distanceModel);
               },
               [](Value self_, std::span<const Value> a) {
                   HostPannerNode* p = pannerOf(self_);
                   if (!p || a.empty() || ev::isObject(a[0]) || ev::isUndefined(a[0])) return ev::undefined();
                   std::string m = ev::toUtf8(a[0]);
                   if (m == "inverse" || m == "linear" || m == "exponential") {
                       p->distanceModel = m;
                   }
                   return ev::undefined();
               });

    b.accessor("refDistance",
               [](Value self_, std::span<const Value>) {
                   HostPannerNode* p = pannerOf(self_);
                   if (!p) return ev::undefined();
                   return ev::fromDouble(p->refDistance);
               },
               [](Value self_, std::span<const Value> a) {
                   HostPannerNode* p = pannerOf(self_);
                   if (!p) return ev::undefined();
                   p->refDistance = static_cast<float>(std::max(0.0, numAt(a, 0)));
                   return ev::undefined();
               });

    b.accessor("maxDistance",
               [](Value self_, std::span<const Value>) {
                   HostPannerNode* p = pannerOf(self_);
                   if (!p) return ev::undefined();
                   return ev::fromDouble(p->maxDistance);
               },
               [](Value self_, std::span<const Value> a) {
                   HostPannerNode* p = pannerOf(self_);
                   if (!p) return ev::undefined();
                   p->maxDistance = static_cast<float>(std::max(0.0, numAt(a, 0)));
                   return ev::undefined();
               });

    b.accessor("rolloffFactor",
               [](Value self_, std::span<const Value>) {
                   HostPannerNode* p = pannerOf(self_);
                   if (!p) return ev::undefined();
                   return ev::fromDouble(p->rolloffFactor);
               },
               [](Value self_, std::span<const Value> a) {
                   HostPannerNode* p = pannerOf(self_);
                   if (!p) return ev::undefined();
                   p->rolloffFactor = static_cast<float>(std::max(0.0, numAt(a, 0)));
                   return ev::undefined();
               });

    b.accessor("coneInnerAngle",
               [](Value self_, std::span<const Value>) {
                   HostPannerNode* p = pannerOf(self_);
                   if (!p) return ev::undefined();
                   return ev::fromDouble(p->coneInnerAngle);
               },
               [](Value self_, std::span<const Value> a) {
                   HostPannerNode* p = pannerOf(self_);
                   if (!p) return ev::undefined();
                   p->coneInnerAngle = static_cast<float>(numAt(a, 0));
                   return ev::undefined();
               });

    b.accessor("coneOuterAngle",
               [](Value self_, std::span<const Value>) {
                   HostPannerNode* p = pannerOf(self_);
                   if (!p) return ev::undefined();
                   return ev::fromDouble(p->coneOuterAngle);
               },
               [](Value self_, std::span<const Value> a) {
                   HostPannerNode* p = pannerOf(self_);
                   if (!p) return ev::undefined();
                   p->coneOuterAngle = static_cast<float>(numAt(a, 0));
                   return ev::undefined();
               });

    b.accessor("coneOuterGain",
               [](Value self_, std::span<const Value>) {
                   HostPannerNode* p = pannerOf(self_);
                   if (!p) return ev::undefined();
                   return ev::fromDouble(p->coneOuterGain);
               },
               [](Value self_, std::span<const Value> a) {
                   HostPannerNode* p = pannerOf(self_);
                   if (!p) return ev::undefined();
                   p->coneOuterGain = static_cast<float>(std::clamp(numAt(a, 0), 0.0, 1.0));
                   return ev::undefined();
               });

    // Legacy positioning methods
    b.def("setPosition", 3, [](Value self_, std::span<const Value> a) -> Value {
        HostPannerNode* p = pannerOf(self_);
        if (!p) return ev::undefined();
        p->posX = static_cast<float>(numAt(a, 0));
        p->posY = static_cast<float>(numAt(a, 1));
        p->posZ = static_cast<float>(numAt(a, 2));

        Value px = ev::getProperty(self_, "positionX");
        Value py = ev::getProperty(self_, "positionY");
        Value pz = ev::getProperty(self_, "positionZ");
        if (auto* pp = hostAudioParamOf(px)) pp->value = p->posX;
        if (auto* pp = hostAudioParamOf(py)) pp->value = p->posY;
        if (auto* pp = hostAudioParamOf(pz)) pp->value = p->posZ;
        return ev::undefined();
    });

    b.def("setOrientation", 3, [](Value self_, std::span<const Value> a) -> Value {
        HostPannerNode* p = pannerOf(self_);
        if (!p) return ev::undefined();
        p->orientX = static_cast<float>(numAt(a, 0));
        p->orientY = static_cast<float>(numAt(a, 1));
        p->orientZ = static_cast<float>(numAt(a, 2));

        Value ox = ev::getProperty(self_, "orientationX");
        Value oy = ev::getProperty(self_, "orientationY");
        Value oz = ev::getProperty(self_, "orientationZ");
        if (auto* pp = hostAudioParamOf(ox)) pp->value = p->orientX;
        if (auto* pp = hostAudioParamOf(oy)) pp->value = p->orientY;
        if (auto* pp = hostAudioParamOf(oz)) pp->value = p->orientZ;
        return ev::undefined();
    });

    b.def("setVelocity", 3, [](Value, std::span<const Value>) -> Value {
        return ev::undefined();
    });
}

Value makePannerNodeValue() {
    auto* panner = new HostPannerNode();
    panner->base.nodeType = AudioNodeType::Panner;

    ObjectBuilder b(g_pannerNodeClass.make(panner, hostPannerDtor));

    b.set("positionX", makeAudioParamValue(AudioParamTarget::PannerPositionX, -1, 0.0f, -3.4e38f, 3.4e38f, 0.0f));
    b.set("positionY", makeAudioParamValue(AudioParamTarget::PannerPositionY, -1, 0.0f, -3.4e38f, 3.4e38f, 0.0f));
    b.set("positionZ", makeAudioParamValue(AudioParamTarget::PannerPositionZ, -1, 0.0f, -3.4e38f, 3.4e38f, 0.0f));

    b.set("orientationX", makeAudioParamValue(AudioParamTarget::PannerOrientationX, -1, 1.0f, -3.4e38f, 3.4e38f, 1.0f));
    b.set("orientationY", makeAudioParamValue(AudioParamTarget::PannerOrientationY, -1, 0.0f, -3.4e38f, 3.4e38f, 0.0f));
    b.set("orientationZ", makeAudioParamValue(AudioParamTarget::PannerOrientationZ, -1, 0.0f, -3.4e38f, 3.4e38f, 0.0f));

    return b.get();
}

// ---------------------------------------------------------------------------
// StereoPannerNode
// ---------------------------------------------------------------------------

void decorateStereoPannerNodeProto(ObjectBuilder&) {
    // pan AudioParam provides all methods/accessors
}

Value makeStereoPannerNodeValue() {
    auto* panner = new HostStereoPannerNode();
    panner->base.nodeType = AudioNodeType::StereoPanner;

    ObjectBuilder b(g_stereoPannerNodeClass.make(panner, hostStereoPannerDtor));

    b.set("pan", makeAudioParamValue(AudioParamTarget::Pan, -1, 0.0f, -1.0f, 1.0f, 0.0f));

    return b.get();
}

}  // namespace bro::bronze_host
