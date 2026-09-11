#include "engine/engine_init_cabi.h"
#include "engine/engine.h"
#include "platform/sdl_window.h"
#include "bro/c_abi/bro_engine_c_abi.h"
#include "js/asset_path.h"
#include "js/dialog_bindings.h"
#include "util/user_dirs.h"
#include "steam/steam_service.h"
#include <broaudio/engine.h>
#include <filesystem>
#include <string>
#include <cstring>

#if BRO_WITH_TEXT_SHAPING
#include "render/bidi.h"
#endif

#if BRO_WITH_PHYSICS
#include "physics/physics_world.h"
#endif

namespace bro::engine {

void bro_engine_register_cabi_bridges(Engine* eng) {
    bro_set_active_engine(eng);

    static BroTimeBridge s_engine_time_bridge = {
        .getTimeScale = []() -> double {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            return eng ? eng->timeScale() : 1.0;
        },
        .setTimeScale = [](double scale) {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (eng) eng->setTimeScale(scale);
        },
        .getTimePaused = []() -> bool {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            return eng ? eng->timePaused() : false;
        },
        .setTimePaused = [](bool paused) {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (eng) eng->setTimePaused(paused);
        },
        .getTimeNowMs = []() -> double {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            return eng ? eng->timeNowMs() : 0.0;
        }
    };
    bro_set_time_bridge(&s_engine_time_bridge);

    static BroPathsBridge s_engine_paths_bridge = {
        .getAppDir = []() -> const char* {
            thread_local std::string s_appDir;
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (!eng || eng->appDir().empty()) return "";
            s_appDir = std::filesystem::path(eng->appDir()).make_preferred().string();
            return s_appDir.c_str();
        },
        .getUserDataDir = []() -> const char* {
            thread_local std::string s_userDataDir;
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (!eng || eng->appDir().empty()) return "";
            std::string dir = util::appUserDataDir(eng->appDir());
            if (!dir.empty()) {
                std::error_code ec;
                std::filesystem::create_directories(dir, ec);
                s_userDataDir = std::filesystem::path(dir).make_preferred().string();
            } else {
                s_userDataDir.clear();
            }
            return s_userDataDir.c_str();
        },
        .resolvePath = [](const char* src) -> const char* {
            thread_local std::string s_resolved;
            if (!src) return "";
            s_resolved = js::resolveAssetPath(src);
            return s_resolved.c_str();
        },
        .resolveWritePath = [](const char* src) -> const char* {
            thread_local std::string s_resolvedWrite;
            if (!src) return "";
            s_resolvedWrite = js::resolveAssetWritePath(src);
            return s_resolvedWrite.c_str();
        }
    };
    bro_set_paths_bridge(&s_engine_paths_bridge);

    static BroDialogsBridge s_engine_dialogs_bridge = {
        .alert = [](const char* message) {
            js::DialogBindings::showAlert(message ? message : "");
        },
        .confirm = [](const char* message) -> bool {
            return js::DialogBindings::showConfirm(message ? message : "");
        },
        .prompt = [](const char* message, const char* defaultText) -> const char* {
            thread_local std::string s_ans;
            auto res = js::DialogBindings::showPrompt(message ? message : "", defaultText ? defaultText : "");
            if (!res) return "";
            s_ans = *res;
            return s_ans.c_str();
        },
        .showSaveFileDialog = [](const char* filter, const char* defaultName) -> const char* {
            (void)filter;
            thread_local std::string s_path;
            s_path = defaultName ? defaultName : "untitled";
            return s_path.c_str();
        },
        .showOpenFileDialog = [](const char* filter, bool allowMultiple) -> const char* {
            thread_local std::string s_path;
            auto picks = js::DialogBindings::pickFiles(filter ? filter : "", allowMultiple);
            if (picks.empty()) return "";
            s_path = picks[0];
            return s_path.c_str();
        },
        .showOpenFolderDialog = [](const char* defaultLocation, bool allowMultiple) -> const char* {
            (void)allowMultiple;
            thread_local std::string s_path;
            s_path = defaultLocation ? defaultLocation : "";
            return s_path.c_str();
        }
    };
    bro_set_dialogs_bridge(&s_engine_dialogs_bridge);

    static BroWindowBridge s_engine_window_bridge = {
        .getState = []() -> const char* {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (!eng || !eng->window()) return "normal";
            if (eng->window()->isMinimized()) return "minimized";
            if (eng->window()->isFullscreen()) return "fullscreen";
            if (eng->window()->isMaximized()) return "maximized";
            return "normal";
        },
        .getBorderless = []() -> bool {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            return (eng && eng->window()) ? eng->window()->isBorderless() : false;
        },
        .setBorderless = [](bool val) {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (eng && eng->window()) eng->window()->setBorderless(val);
        },
        .getAlwaysOnTop = []() -> bool {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            return (eng && eng->window()) ? eng->window()->isAlwaysOnTop() : false;
        },
        .setAlwaysOnTop = [](bool val) {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (eng && eng->window()) eng->window()->setAlwaysOnTop(val);
        },
        .minimize = []() {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (eng && eng->window() && eng->displayMode() != DisplayMode::Headless)
                eng->window()->minimize();
        },
        .maximize = []() {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (eng && eng->window() && eng->displayMode() != DisplayMode::Headless)
                eng->window()->maximize();
        },
        .restore = []() {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (eng && eng->window() && eng->displayMode() != DisplayMode::Headless)
                eng->window()->restore();
        },
        .getPositionX = []() -> int32_t {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (!eng || !eng->window()) return 0;
            int x = 0, y = 0;
            eng->window()->getPosition(x, y);
            return x;
        },
        .getPositionY = []() -> int32_t {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (!eng || !eng->window()) return 0;
            int x = 0, y = 0;
            eng->window()->getPosition(x, y);
            return y;
        },
        .setPosition = [](int32_t x, int32_t y) {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (eng && eng->window() && eng->displayMode() != DisplayMode::Headless)
                eng->window()->setPosition(x, y);
        },
        .getMinWidth = []() -> int32_t {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (!eng || !eng->window()) return 0;
            int w = 0, h = 0;
            eng->window()->getMinimumSize(w, h);
            return w;
        },
        .getMinHeight = []() -> int32_t {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (!eng || !eng->window()) return 0;
            int w = 0, h = 0;
            eng->window()->getMinimumSize(w, h);
            return h;
        },
        .setMinSize = [](int32_t width, int32_t height) {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (eng && eng->window()) eng->window()->setMinimumSize(width, height);
        },
        .getMaxWidth = []() -> int32_t {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (!eng || !eng->window()) return 0;
            int w = 0, h = 0;
            eng->window()->getMaximumSize(w, h);
            return w;
        },
        .getMaxHeight = []() -> int32_t {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (!eng || !eng->window()) return 0;
            int w = 0, h = 0;
            eng->window()->getMaximumSize(w, h);
            return h;
        },
        .setMaxSize = [](int32_t width, int32_t height) {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (eng && eng->window()) eng->window()->setMaximumSize(width, height);
        },
        .getDisplayCount = []() -> int32_t {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (!eng || !eng->window()) return 1;
            return static_cast<int32_t>(eng->window()->getDisplays().size());
        },
        .moveToDisplay = [](uint32_t id) -> bool {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (!eng || !eng->window() || eng->displayMode() == DisplayMode::Headless)
                return false;
            return eng->window()->moveToDisplay(id);
        }
    };
    bro_set_window_bridge(&s_engine_window_bridge);

    static BroSettingsBridge s_engine_settings_bridge = {
        .load = []() {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (eng && eng->settings()) eng->settings()->load();
        },
        .save = []() {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (eng && eng->settings()) eng->settings()->save();
        },
        .get = [](const char* key) -> const char* {
            thread_local std::string s_val;
            if (!key) return "";
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (!eng || !eng->settings()) return "";
            s_val = eng->settings()->getString(key);
            return s_val.c_str();
        },
        .set = [](const char* key, const char* val) {
            if (!key || !val) return;
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (eng && eng->settings()) eng->settings()->setUser(key, val);
        },
        .reset = [](const char* category) {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (!eng || !eng->settings()) return;
            if (category && *category) {
                eng->settings()->resetCategory(category);
            } else {
                eng->settings()->resetAll();
            }
        },
        .isActionPressed = [](const char* action) -> bool {
            if (!action) return false;
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            return eng ? eng->actionPressed(action) : false;
        },
        .getActionStrength = [](const char* action) -> double {
            if (!action) return 0.0;
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            return eng ? static_cast<double>(eng->actionStrength(action)) : 0.0;
        }
    };
    bro_set_settings_bridge(&s_engine_settings_bridge);

    static BroMenuBridge s_engine_menu_bridge = {
        .getVisible = []() -> bool {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            return eng ? eng->menuBar().visible : false;
        },
        .show = []() {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (eng) {
                eng->menuBar().visible = true;
                eng->menuBar().dirty = true;
                eng->onMenuChanged();
            }
        },
        .hide = []() {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (eng) {
                eng->menuBar().visible = false;
                eng->menuBar().dirty = true;
                eng->onMenuChanged();
            }
        },
        .set = [](void*) {},
        .addItem = [](const char*, void*, int32_t) -> bool { return true; },
        .updateItem = [](const char*, void*) -> bool { return true; },
        .removeItem = [](const char* id) -> bool {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (!eng || !id) return false;
            bool ok = eng->menuBar().removeItem(id);
            if (ok) eng->onMenuChanged();
            return ok;
        },
        .on = [](const char*, void*) {}
    };
    bro_set_menu_bridge(&s_engine_menu_bridge);

    static BroMicBridge s_engine_mic_bridge = {
        .start = [](void*) {},
        .stop = []() {},
        .isActive = []() -> bool { return false; },
        .engineRate = []() -> int32_t {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            return (eng && eng->audioEngine()) ? eng->audioEngine()->sampleRate() : 48000;
        },
        .stats = []() -> void* { return nullptr; },
        .levels = [](int32_t) -> void* { return nullptr; },
        .feed = [](void*, int32_t) {}
    };
    bro_set_mic_bridge(&s_engine_mic_bridge);

    static BroGamepadBridge s_engine_gamepad_bridge = {
        .isConnected = [](int32_t index) -> bool {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (!eng || index < 0) return false;
            const auto& pads = eng->gamepads();
            if (static_cast<size_t>(index) < pads.size()) {
                return pads[static_cast<size_t>(index)].connected;
            }
            return false;
        },
        .getAxis = [](int32_t index, int32_t axis) -> double {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (!eng || index < 0 || axis < 0 || axis >= kGamepadAxisCount) return 0.0;
            const auto& pads = eng->gamepads();
            if (static_cast<size_t>(index) < pads.size() && pads[static_cast<size_t>(index)].connected) {
                return static_cast<double>(pads[static_cast<size_t>(index)].axes[axis]);
            }
            return 0.0;
        },
        .getButton = [](int32_t index, int32_t button) -> double {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (!eng || index < 0 || button < 0 || button >= kGamepadButtonCount) return 0.0;
            const auto& pads = eng->gamepads();
            if (static_cast<size_t>(index) < pads.size() && pads[static_cast<size_t>(index)].connected) {
                return static_cast<double>(pads[static_cast<size_t>(index)].buttons[button]);
            }
            return 0.0;
        },
        .rumble = [](int32_t index, float strong, float weak, int32_t duration) -> bool {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            return eng ? eng->gamepadRumble(index, strong, weak, duration) : false;
        },
        .rumbleTriggers = [](int32_t index, float left, float right, int32_t duration) -> bool {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            return eng ? eng->gamepadRumbleTriggers(index, left, right, duration) : false;
        }
    };
    bro_set_gamepad_bridge(&s_engine_gamepad_bridge);

    static BroMediaBridge s_engine_media_bridge = {
        .getAvailable = []() -> bool { return true; },
        .peaks = [](const char*, void*) -> void* { return nullptr; },
        .thumbnails = [](const char*, void*) -> void* { return nullptr; }
    };
    bro_set_media_bridge(&s_engine_media_bridge);

    static BroListenBridge s_engine_listen_bridge = {
        .open = [](void*) -> void* { return nullptr; },
        .supported = []() -> bool { return true; },
        .apps = []() -> void* { return nullptr; },
        .retain = [](int32_t) {},
        .audio = [](int64_t, int64_t) -> void* { return nullptr; },
        .frame = []() -> int64_t { return 0; },
        .info = []() -> void* { return nullptr; }
    };
    bro_set_listen_bridge(&s_engine_listen_bridge);

    static BroSteamBridge s_engine_steam_bridge = {
        .getAvailable = []() -> bool {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            return eng && eng->steamService() && eng->steamService()->available();
        },
        .getReason = []() -> const char* {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            return (eng && eng->steamService()) ? eng->steamService()->reason() : "bro.steam not initialized";
        },
        .getAppId = []() -> uint32_t {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            return (eng && eng->steamService() && eng->steamService()->available()) ? eng->steamService()->appId() : 0;
        },
        .getSteamId = []() -> const char* {
            static std::string s_steamIdStr;
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            uint64_t id = (eng && eng->steamService() && eng->steamService()->available()) ? eng->steamService()->localSteamId() : 0;
            s_steamIdStr = std::to_string(id);
            return s_steamIdStr.c_str();
        },
        .getPersonaName = []() -> const char* {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (eng && eng->steamService() && eng->steamService()->available()) {
                return eng->steamService()->personaName().c_str();
            }
            return "";
        },
        .getIsLoggedOn = []() -> bool {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            return eng && eng->steamService() && eng->steamService()->available();
        },
        .getIsVoiceRecording = []() -> bool {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            return eng && eng->steamService() && eng->steamService()->voiceRecording();
        },
        .getVoiceSampleRate = []() -> int32_t {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            return (eng && eng->steamService()) ? static_cast<int32_t>(eng->steamService()->voiceSampleRate()) : 0;
        },
        .getAchievement = [](const char*) -> bool { return false; },
        .setAchievement = [](const char*) -> bool { return false; },
        .clearAchievement = [](const char*) -> bool { return false; },
        .getStat = [](const char*) -> double { return 0.0; },
        .setStat = [](const char*, double) -> bool { return false; },
        .storeStats = []() -> bool { return false; },
        .activateOverlay = [](const char* dialog) {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (eng && eng->steamService()) eng->steamService()->activateOverlay(dialog ? dialog : "");
        },
        .activateOverlayToWebPage = [](const char*) {},
        .getFriends = []() -> void* { return nullptr; },
        .getAvatar = [](const char*, void*) -> void* { return nullptr; },
        .setRichPresence = [](const char* key, const char* value) -> bool {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (eng && eng->steamService() && key && value) {
                eng->steamService()->setRichPresence(key, value);
                return true;
            }
            return false;
        },
        .clearRichPresence = []() {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (eng && eng->steamService()) eng->steamService()->clearRichPresence();
        },
        .createLobby = [](const char*, int32_t) -> void* { return nullptr; },
        .joinLobby = [](const char*) -> void* { return nullptr; },
        .leaveLobby = [](const char* lobbyId) {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (eng && eng->steamService() && lobbyId) {
                try {
                    uint64_t id = std::stoull(lobbyId);
                    eng->steamService()->leaveLobby(id);
                } catch (...) {}
            }
        },
        .setLobbyData = [](const char*, const char*, const char*) -> bool { return false; },
        .getLobbyMembers = [](const char*) -> void* { return nullptr; },
        .getLobbyOwner = [](const char*) -> const char* { return "0"; },
        .getLobbyData = [](const char*, const char*) -> const char* { return ""; },
        .requestLobbyList = [](void*) {},
        .inviteUserToLobby = [](const char*, const char*) -> bool { return false; },
        .startVoiceRecording = []() {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (eng && eng->steamService()) eng->steamService()->startVoiceRecording();
        },
        .stopVoiceRecording = []() {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (eng && eng->steamService()) eng->steamService()->stopVoiceRecording();
        },
        .decodeVoice = [](void*, int32_t) -> void* { return nullptr; }
    };
    bro_set_steam_bridge(&s_engine_steam_bridge);

    static BroServerBridge s_engine_server_bridge = {
        .getTickrate = []() -> double {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            return eng ? eng->serverTickRate() : 60.0;
        },
        .setTickrate = [](double val) {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (eng) {
                if (val < 1.0) val = 1.0;
                if (val > 1000.0) val = 1000.0;
                eng->setServerTickRate(val);
            }
        },
        .getUptime = []() -> double {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            return eng ? eng->serverUptime() : 0.0;
        },
        .stop = []() {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (eng) eng->requestServerStop();
        }
    };
    bro_set_server_bridge(&s_engine_server_bridge);

    static BroNetBridge s_engine_net_bridge = {
        .host = [](int32_t, void*) {},
        .unhost = []() {},
        .connect = [](const char*, int32_t, void*) -> int32_t { return 0; },
        .disconnect = [](int32_t) {},
        .disconnectAll = []() {},
        .send = [](int32_t, void*, int32_t) {},
        .broadcast = [](void*, int32_t) {},
        .sendClone = [](int32_t, void*, int32_t) {},
        .broadcastClone = [](void*, int32_t) {},
        .peers = []() -> void* { return nullptr; },
        .getPeerAddress = [](int32_t) -> const char* { return ""; },
        .stats = []() -> void* { return nullptr; },
        .getPeerStats = [](int32_t) -> void* { return nullptr; },
        .setPeerSimulatedLoss = [](int32_t, double, double, double) {}
    };
    bro_set_net_bridge(&s_engine_net_bridge);

    static BroTextBridge s_engine_text_bridge = {
        .getBidiAvailable = []() -> bool {
#if BRO_WITH_TEXT_SHAPING
            return render::bidi::available();
#else
            return false;
#endif
        },
        .shape = [](const char*, void*) -> void* { return nullptr; },
        .byteOffsetToX = [](const char*, void*, int32_t) -> void* { return nullptr; },
        .xToByteOffset = [](const char*, void*, double) -> int32_t { return 0; },
        .clusterRange = [](const char*, void*, int32_t) -> void* { return nullptr; },
        .cacheStats = []() -> void* { return nullptr; },
        .bidi = [](const char*, const char*, bool) -> void* { return nullptr; },
        .bidiReorder = [](void*) -> void* { return nullptr; }
    };
    bro_set_text_bridge(&s_engine_text_bridge);

    static BroGpuBridge s_engine_gpu_bridge = {
        .getAvailable = []() -> bool { return false; },
        .getBackend = []() -> const char* { return "cpu"; },
        .getDevices = []() -> void* { return nullptr; },
        .deviceCount = [](const char* device) -> int32_t {
            return (!device || (device && std::strcmp(device, "cpu") == 0)) ? 1 : 0;
        },
        .getCompiledBackends = []() -> void* { return nullptr; },
        .memoryInfo = [](const char*) -> void* { return nullptr; },
        .deviceName = [](const char* device) -> const char* {
            (void)device;
            return "cpu";
        },
        .trim = [](const char*, uint64_t) -> bool { return false; }
    };
    bro_set_gpu_bridge(&s_engine_gpu_bridge);

    static BroGizmoBridge s_engine_gizmo_bridge = {
        .getVisible = []() -> bool {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            return eng ? eng->gizmo().visible() : false;
        },
        .getDragging = []() -> bool {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            return eng ? eng->gizmo().isDragging() : false;
        },
        .getHovered = []() -> const char* {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (!eng) return nullptr;
            switch (eng->gizmo().hovered()) {
                case GizmoAxis::X: return "x";
                case GizmoAxis::Y: return "y";
                case GizmoAxis::Z: return "z";
                case GizmoAxis::XY: return "xy";
                case GizmoAxis::YZ: return "yz";
                case GizmoAxis::XZ: return "xz";
                case GizmoAxis::View: return "view";
                case GizmoAxis::Center: return "center";
                default: return nullptr;
            }
        },
        .show = []() {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (eng) eng->gizmo().show();
        },
        .hide = []() {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (eng) eng->gizmo().hide();
        },
        .setMode = [](const char* mode) {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (!eng || !mode) return;
            if (std::strcmp(mode, "translate") == 0)      eng->gizmo().setMode(GizmoMode::Translate);
            else if (std::strcmp(mode, "rotate") == 0)    eng->gizmo().setMode(GizmoMode::Rotate);
            else if (std::strcmp(mode, "scale") == 0)     eng->gizmo().setMode(GizmoMode::Scale);
        },
        .setSpace = [](const char* space) {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (!eng || !space) return;
            if (std::strcmp(space, "local") == 0) eng->gizmo().setSpace(GizmoSpace::Local);
            else                                  eng->gizmo().setSpace(GizmoSpace::World);
        },
        .setPosition = [](double x, double y, double z) {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (eng) eng->gizmo().setPosition(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
        },
        .setOrientation = [](double x, double y, double z, double w) {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (eng) eng->gizmo().setOrientation(bromath::Quat(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), static_cast<float>(w)));
        },
        .configure = [](void* /*config*/) {},
        .attach = [](void* /*handlers*/) {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (eng) eng->gizmo().show();
        },
        .detach = []() {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
            if (eng) eng->gizmo().hide();
        }
    };
    bro_set_gizmo_bridge(&s_engine_gizmo_bridge);

    static BroPhysicsBridge s_engine_physics_bridge = {
        .setGravity = [](double x, double y, double z) {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
#if BRO_WITH_PHYSICS
            if (eng && eng->physicsWorld()) {
                eng->physicsWorld()->setGravity(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
            }
#else
            (void)eng; (void)x; (void)y; (void)z;
#endif
        },
        .getGravity = []() -> void* {
            static double s_g[3] = {0.0, -9.81, 0.0};
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
#if BRO_WITH_PHYSICS
            if (eng && eng->physicsWorld()) {
                auto g = eng->physicsWorld()->gravity();
                s_g[0] = static_cast<double>(g.GetX());
                s_g[1] = static_cast<double>(g.GetY());
                s_g[2] = static_cast<double>(g.GetZ());
            }
#else
            (void)eng;
#endif
            return s_g;
        },
        .createBody = [](void* /*config*/) -> int32_t { return 0; },
        .destroyBody = [](int32_t /*tag*/) {},
        .destroyAll = []() {},
        .getTransform = [](int32_t /*tag*/) -> void* { return nullptr; },
        .getVelocity = [](int32_t /*tag*/) -> void* { return nullptr; },
        .setPosition = [](int32_t /*tag*/, double /*x*/, double /*y*/, double /*z*/) {},
        .setRotation = [](int32_t /*tag*/, double /*x*/, double /*y*/, double /*z*/, double /*w*/) {},
        .setLinearVelocity = [](int32_t /*tag*/, double /*x*/, double /*y*/, double /*z*/) {},
        .setAngularVelocity = [](int32_t /*tag*/, double /*x*/, double /*y*/, double /*z*/) {},
        .addForce = [](int32_t /*tag*/, double /*x*/, double /*y*/, double /*z*/) {},
        .addImpulse = [](int32_t /*tag*/, double /*x*/, double /*y*/, double /*z*/) {},
        .addTorque = [](int32_t /*tag*/, double /*x*/, double /*y*/, double /*z*/) {},
        .raycast = [](double /*ox*/, double /*oy*/, double /*oz*/, double /*dx*/, double /*dy*/, double /*dz*/, double /*maxDist*/, int32_t /*mask*/) -> void* { return nullptr; },
        .raycastClosest = [](double /*ox*/, double /*oy*/, double /*oz*/, double /*dx*/, double /*dy*/, double /*dz*/, double /*maxDist*/, int32_t /*mask*/) -> void* { return nullptr; },
        .step = [](double /*dt*/) {},
        .setTimeStep = [](double dt) {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
#if BRO_WITH_PHYSICS
            if (eng && eng->physicsWorld()) {
                eng->physicsWorld()->setTimeStep(static_cast<float>(dt));
            }
#else
            (void)eng; (void)dt;
#endif
        },
        .setInterpolation = [](bool enabled) {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
#if BRO_WITH_PHYSICS
            if (eng && eng->physicsWorld()) {
                eng->physicsWorld()->setInterpolation(enabled);
            }
#else
            (void)eng; (void)enabled;
#endif
        },
        .getInterpolation = []() -> bool {
            auto* eng = static_cast<Engine*>(bro_get_active_engine());
#if BRO_WITH_PHYSICS
            if (eng && eng->physicsWorld()) {
                return eng->physicsWorld()->interpolation();
            }
#else
            (void)eng;
#endif
            return false;
        },
        .isActive = [](int32_t /*tag*/) -> bool { return false; },
        .activate = [](int32_t /*tag*/) {},
        .createCharacter = [](void* /*config*/) -> void* { return nullptr; },
        .createVehicle = [](void* /*config*/) -> void* { return nullptr; },
        .createRagdoll = [](void* /*config*/) -> void* { return nullptr; },
        .createSoftBody = [](void* /*config*/) -> void* { return nullptr; }
    };
    bro_set_physics_bridge(&s_engine_physics_bridge);

    static BroFloraBridge s_engine_flora_bridge = {
        .setWind = [](double /*strength*/, double /*dirX*/, double /*dirY*/) {},
        .setDensity = [](double /*density*/) {},
        .update = [](double /*dt*/) {},
        .clear = []() {},
        .placement = [](void* /*config*/) {},
        .batches = []() -> void* { return nullptr; },
        .createWorld = [](void* /*opts*/) -> void* { return nullptr; }
    };
    bro_set_flora_bridge(&s_engine_flora_bridge);

    static BroMotionBridge s_engine_motion_bridge = {
        .init = []() {},
        .load = [](void* /*opts*/) -> void* { return nullptr; }
    };
    bro_set_motion_bridge(&s_engine_motion_bridge);

    static BroAIBridge s_engine_ai_bridge = {
        .createWorld = [](void* /*opts*/) -> void* { return nullptr; },
        .createAgent = [](void* /*world*/, void* /*opts*/) -> void* { return nullptr; },
        .step = [](void* /*world*/, double /*dt*/) {}
    };
    bro_set_ai_bridge(&s_engine_ai_bridge);

    static BroTensorBridge s_engine_tensor_bridge = {
        .init = []() {},
        .sync = []() {},
        .getAvailable = []() -> bool { return true; },
        .getBackend = []() -> const char* { return "cpu"; }
    };
    bro_set_tensor_bridge(&s_engine_tensor_bridge);

    static BroVisionBridge s_engine_vision_bridge = {
        .init = []() {},
        .loadDepth = [](const char* /*modelDir*/, void* /*opts*/) -> void* { return nullptr; }
    };
    bro_set_vision_bridge(&s_engine_vision_bridge);

    static BroDiffusionBridge s_engine_diffusion_bridge = {
        .init = []() {},
        .createPipeline = [](void* /*config*/) -> void* { return nullptr; },
        .loadModel = [](const char* /*dir*/, void* /*opts*/) -> void* { return nullptr; }
    };
    bro_set_diffusion_bridge(&s_engine_diffusion_bridge);
}

} // namespace bro::engine