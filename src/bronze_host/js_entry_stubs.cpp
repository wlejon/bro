// No-op entry points for bro's bronze-compiled JS modules, linked in TWO
// places instead of the compiled objects:
//
//   * bro-native-manifest (native_manifest_tool.cpp), the build-time tool
//     that prints the manifest js/bro_core.js is compiled against. It links
//     bro_bronze_host, whose host_js_modules.cpp names every entry — and the
//     compiled objects cannot exist yet, because one of them is the output
//     the tool's manifest is an input to. Nothing in the tool runs an entry.
//
//   * Apple Silicon, mirroring brokit's bronze_js_stubs_arm64.cpp: bronze's
//     code generator (brass) emits x86_64, and an x86_64 object cannot be
//     linked into an arm64 Mach-O image. There no bronze-compiled JS — an
//     app's module included — can run at all.
//
// Everywhere else the executables link bro_bronze_js, the real objects from
// js/, and this file is not in their link.

extern "C" {
void bro_observers_main() {}
void bro_events_main() {}
void bro_net_sync_main() {}
void bro_image_gpu_main() {}
void bro_core_main() {}
void bro_net_main() {}
void bro_physics_main() {}
void bro_terrain_main() {}
void bro_clipmap_main() {}
void bro_tile_world_main() {}
void bro_lighting_main() {}
void bro_gizmo_main() {}
void bro_animation_main() {}
void bro_scene_main() {}
void bro_scene_extras_main() {}
void bro_lm_main() {}
void bro_rave_main() {}
void bro_motion_main() {}
void bro_server_main() {}
void bro_mic_main() {}
void bro_gesture_main() {}
void bro_sense_main() {}
void bro_wake_main() {}
void bro_kws_main() {}
void bro_listen_main() {}
void bro_triposplat_main() {}
void bro_diffusion_main() {}
void bro_vision_main() {}
void bro_diar_main() {}
void bro_stt_main() {}
void bro_tts_main() {}
void bro_flora_main() {}
void bro_tensor_main() {}
void bro_impostor_main() {}
}
