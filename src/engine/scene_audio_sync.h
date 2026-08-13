#pragma once

#include <memory>

#if BRO_WITH_3D
#include "scene/scene_graph.h"
#else
namespace bro::scene {
class SceneNode;
class SceneGraph {
public:
    struct LivenessToken {};
};
}
#endif

namespace broaudio { class Engine; }

namespace bro::engine {

class SceneAudioSync {
public:
    static void install(broaudio::Engine* engine);
    static void shutdown();
    static void sync(float dtSec);

    static void attachAudioEmitter(std::weak_ptr<bro::scene::SceneGraph::LivenessToken> token,
                                   bro::scene::SceneNode* node, int handle, bool isVoice);
    static void detachAudioEmitter(uint32_t nodeId);
    static void bindAudioListenerToCamera(std::weak_ptr<bro::scene::SceneGraph::LivenessToken> token,
                                         bro::scene::SceneGraph* graph, bool enable);
};

} // namespace bro::engine
