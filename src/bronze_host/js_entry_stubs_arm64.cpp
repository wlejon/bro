// Native arm64 entry points for bro's bronze-compiled JS modules on Apple
// Silicon, mirroring brokit's bronze_js_stubs_arm64.cpp for the same reason:
// bronze's code generator (brass) emits x86_64, and an x86_64 object cannot
// be linked into an arm64 Mach-O image. CMakeLists.txt links this file in
// place of the compiled objects on that one platform, where no bronze-compiled
// JS — an app's module included — can run at all. Everywhere else the real
// objects from js/ are linked and this file is not compiled.

extern "C" {
void bro_observers_main() {}
void bro_net_sync_main() {}
void bro_image_gpu_main() {}
}
