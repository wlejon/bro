#pragma once

// Forward declarations of the collaborators Engine holds by pointer or names
// in its interface, so engine.h does not have to pull in their headers.

typedef struct SDL_GLContextState* SDL_GLContext;

namespace bro::layout { struct KeyHandleResult; }
namespace bro::render {
    class GLContext;
    class RasterRenderer;
    class RecordingRenderer;
    class CommandReplayer;
    class CommandBuffer;
    class Renderer;
}
namespace bro::webgl { class WebGL2RenderingContext; }
namespace broaudio { class Engine; }
namespace bro::physics { class PhysicsWorld; }
namespace bro::net { class NetService; }
namespace bro::steam { class SteamService; }
namespace bro::scene { class SceneGraph; class HtmlNode; struct CullStats; }
namespace bro::canvas { class CanvasScene; class CanvasRasterThread; }
namespace bro::platform { class Window; class EventLoop; }
namespace bro::dom { class Document; class Element; class Event; class TextNode; }
namespace bro::layout { class DrawTraversal; class SkiaTextMetrics; }
namespace brokit::api { class FsWatcher; }

namespace bro::engine {

class Engine;
class LayoutPipeline;
class AudioInference;
struct SubDocRef;

} // namespace bro::engine
