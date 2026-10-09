#include "platform/sdl/sdl_runtime.h"
#include "util/log.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include <cstring>
#include <vector>

namespace bro::platform {

// Main-thread only — see header.
static int s_refCount = 0;
static bool s_offscreenFallback = false;

namespace {

// Does the Vulkan loader offer VK_EXT_headless_surface, the surface SDL's
// offscreen driver creates? Asked of the loader SDL itself would use.
bool vulkanHasHeadlessSurface() {
    if (!SDL_Vulkan_LoadLibrary(nullptr)) return false;
    bool found = false;
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    auto enumerate = gipa ? reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
                                gipa(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties"))
                          : nullptr;
    uint32_t n = 0;
    if (enumerate && enumerate(nullptr, &n, nullptr) == VK_SUCCESS) {
        std::vector<VkExtensionProperties> exts(n);
        if (enumerate(nullptr, &n, exts.data()) == VK_SUCCESS)
            for (const auto& e : exts)
                if (std::strcmp(e.extensionName, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME) == 0) found = true;
    }
    SDL_Vulkan_UnloadLibrary();
    return found;
}

}  // namespace

bool SdlRuntime::offscreenFallback() { return s_offscreenFallback; }

bool SdlRuntime::acquire() {
    if (s_refCount == 0) {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            LOG_ERROR("Failed to initialize SDL: %s", SDL_GetError());
            return false;
        }
        // The offscreen driver's windows present through
        // VK_EXT_headless_surface. Where the GPU driver lacks it, the
        // windows could never get a swapchain: use the platform's video
        // driver instead, with every window kept hidden — the same frame
        // loop, swapchain and present readback, on a real (unshown) surface.
        const char* driver = SDL_GetCurrentVideoDriver();
        if (driver && std::strcmp(driver, "offscreen") == 0 && !vulkanHasHeadlessSurface()) {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "", SDL_HINT_OVERRIDE);
            if (!SDL_Init(SDL_INIT_VIDEO)) {
                LOG_ERROR("Failed to initialize SDL without the offscreen driver: %s", SDL_GetError());
                return false;
            }
            s_offscreenFallback = true;
            LOG_WARN("SDL offscreen video driver: this Vulkan driver has no VK_EXT_headless_surface; "
                     "standing in with hidden windows on the '%s' driver", SDL_GetCurrentVideoDriver());
        }

        // Gamepad support is best-effort: a headless box or stripped-down
        // driver stack may have no controller backend, and apps must still run
        // (they just see zero gamepads). So init it as a subsystem and only
        // log on failure.
        if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
            LOG_INFO("SDL gamepad subsystem unavailable: %s", SDL_GetError());
        }

        // Deliver the click that activates an unfocused window. SDL defaults
        // this off on every platform (Windows/X11/Wayland all gate on the same
        // hint), so without it the first click after alt-tabbing back in is
        // swallowed to raise the window and the user has to click a second
        // time to actually hit anything. Browsers and native apps pass that
        // click through; so do we.
        SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    }
    ++s_refCount;
    return true;
}

void SdlRuntime::release() {
    if (s_refCount <= 0) {
        LOG_ERROR("SdlRuntime::release() without matching acquire()");
        return;
    }
    if (--s_refCount == 0) {
        // s_offscreenFallback stays: the driver override does too, so a
        // later init lands on the same hidden native windows.
        SDL_Quit();
    }
}

} // namespace bro::platform
