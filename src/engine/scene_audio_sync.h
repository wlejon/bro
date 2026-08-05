#pragma once

#include <memory>
#include "scene/scene_graph.h"

namespace broaudio { class Engine; }

namespace bro::engine {

class SceneAudioSync {
public:
    static void install(broaudio::Engine* engine);
    static void shutdown();
    static void sync(float dtSec);

    static void attachAudioEmitter(std::weak_ptr<scene::SceneGraph::LivenessToken> token,
                                   scene::SceneNode* node, int handle, bool isVoice);
    static void detachAudioEmitter(uint32_t nodeId);
    static void bindAudioListenerToCamera(std::weak_ptr<scene::SceneGraph::LivenessToken> token,
                                         scene::SceneGraph* graph, bool enable);
};

} // namespace bro::engine
