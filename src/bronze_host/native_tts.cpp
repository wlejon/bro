// native_tts.cpp — Bronze native entry points for bro.tts
#include "natives/tts/native_tts_decl.h"

#include <cstdint>
#include <string>

namespace {

struct BroKokoroModelImpl { bool loaded = true; std::string device = "CPU"; };
struct BroVoiceImpl { bool loaded = true; std::string name = "af"; };
struct BroKokoroSessionImpl { bool loaded = true; };
struct BroQwenTtsModelImpl { bool loaded = true; std::string device = "CPU"; std::string variant = "base"; };
struct BroQwenTtsSessionImpl { bool loaded = true; std::string variant = "base"; };
struct BroSupertonicModelImpl { bool loaded = true; std::string device = "CPU"; };
struct BroSupertonicVoiceImpl { bool loaded = true; std::string name = "default"; };
struct BroSpeakerEncoderImpl { bool loaded = true; std::string device = "CPU"; };

}  // namespace

extern "C" {

// KokoroModel
void bro_tts_KokoroModel_dtor(void* self) {
    delete static_cast<BroKokoroModelImpl*>(self);
}
void* bro_tts_KokoroModel_ctor(void) {
    return new BroKokoroModelImpl();
}
bool bro_tts_KokoroModel_loaded_get(void* self) {
    return self && static_cast<BroKokoroModelImpl*>(self)->loaded;
}
const char* bro_tts_KokoroModel_device_get(void* self) {
    return self ? static_cast<BroKokoroModelImpl*>(self)->device.c_str() : "CPU";
}
void* bro_tts_KokoroModel_loadVoice(void* /*self*/, const char* /*path*/) {
    return new BroVoiceImpl();
}
void* bro_tts_KokoroModel_createSession(void* /*self*/) {
    return new BroKokoroSessionImpl();
}

// Voice
void bro_tts_Voice_dtor(void* self) {
    delete static_cast<BroVoiceImpl*>(self);
}
void* bro_tts_Voice_ctor(void) {
    return new BroVoiceImpl();
}
bool bro_tts_Voice_loaded_get(void* self) {
    return self && static_cast<BroVoiceImpl*>(self)->loaded;
}
const char* bro_tts_Voice_name_get(void* self) {
    return self ? static_cast<BroVoiceImpl*>(self)->name.c_str() : "af";
}

// KokoroSession
void bro_tts_KokoroSession_dtor(void* self) {
    delete static_cast<BroKokoroSessionImpl*>(self);
}
void* bro_tts_KokoroSession_ctor(void) {
    return new BroKokoroSessionImpl();
}
bool bro_tts_KokoroSession_loaded_get(void* self) {
    return self && static_cast<BroKokoroSessionImpl*>(self)->loaded;
}
void bro_tts_KokoroSession_reset(void* /*self*/) {
}

// QwenTtsModel
void bro_tts_QwenTtsModel_dtor(void* self) {
    delete static_cast<BroQwenTtsModelImpl*>(self);
}
void* bro_tts_QwenTtsModel_ctor(void) {
    return new BroQwenTtsModelImpl();
}
bool bro_tts_QwenTtsModel_loaded_get(void* self) {
    return self && static_cast<BroQwenTtsModelImpl*>(self)->loaded;
}
const char* bro_tts_QwenTtsModel_device_get(void* self) {
    return self ? static_cast<BroQwenTtsModelImpl*>(self)->device.c_str() : "CPU";
}
const char* bro_tts_QwenTtsModel_variant_get(void* self) {
    return self ? static_cast<BroQwenTtsModelImpl*>(self)->variant.c_str() : "base";
}
void* bro_tts_QwenTtsModel_createSession(void* /*self*/) {
    return new BroQwenTtsSessionImpl();
}

// QwenTtsSession
void bro_tts_QwenTtsSession_dtor(void* self) {
    delete static_cast<BroQwenTtsSessionImpl*>(self);
}
void* bro_tts_QwenTtsSession_ctor(void) {
    return new BroQwenTtsSessionImpl();
}
bool bro_tts_QwenTtsSession_loaded_get(void* self) {
    return self && static_cast<BroQwenTtsSessionImpl*>(self)->loaded;
}
const char* bro_tts_QwenTtsSession_variant_get(void* self) {
    return self ? static_cast<BroQwenTtsSessionImpl*>(self)->variant.c_str() : "base";
}
void bro_tts_QwenTtsSession_reset(void* /*self*/) {
}

// SupertonicModel
void bro_tts_SupertonicModel_dtor(void* self) {
    delete static_cast<BroSupertonicModelImpl*>(self);
}
void* bro_tts_SupertonicModel_ctor(void) {
    return new BroSupertonicModelImpl();
}
bool bro_tts_SupertonicModel_loaded_get(void* self) {
    return self && static_cast<BroSupertonicModelImpl*>(self)->loaded;
}
const char* bro_tts_SupertonicModel_device_get(void* self) {
    return self ? static_cast<BroSupertonicModelImpl*>(self)->device.c_str() : "CPU";
}
void* bro_tts_SupertonicModel_loadVoiceStyle(void* /*self*/, const char* /*path*/) {
    return new BroSupertonicVoiceImpl();
}

// SupertonicVoice
void bro_tts_SupertonicVoice_dtor(void* self) {
    delete static_cast<BroSupertonicVoiceImpl*>(self);
}
void* bro_tts_SupertonicVoice_ctor(void) {
    return new BroSupertonicVoiceImpl();
}
bool bro_tts_SupertonicVoice_loaded_get(void* self) {
    return self && static_cast<BroSupertonicVoiceImpl*>(self)->loaded;
}
const char* bro_tts_SupertonicVoice_name_get(void* self) {
    return self ? static_cast<BroSupertonicVoiceImpl*>(self)->name.c_str() : "default";
}

// SpeakerEncoder
void bro_tts_SpeakerEncoder_dtor(void* self) {
    delete static_cast<BroSpeakerEncoderImpl*>(self);
}
void* bro_tts_SpeakerEncoder_ctor(void) {
    return new BroSpeakerEncoderImpl();
}
bool bro_tts_SpeakerEncoder_loaded_get(void* self) {
    return self && static_cast<BroSpeakerEncoderImpl*>(self)->loaded;
}
const char* bro_tts_SpeakerEncoder_device_get(void* self) {
    return self ? static_cast<BroSpeakerEncoderImpl*>(self)->device.c_str() : "CPU";
}

// Namespace
void bro_tts_init(void) {
}

void bro_tts_setAssetRoot(const char* /*dir*/) {
}

void bro_tts_setAssets(bool /*opts_root_given*/, const char* /*opts_root*/,
                       bool /*opts_lexicon_given*/, const char* /*opts_lexicon*/,
                       bool /*opts_pos_given*/, const char* /*opts_pos*/,
                       bool /*opts_kokoroConfig_given*/, const char* /*opts_kokoroConfig*/) {
}

}  // extern "C"

namespace bro::bronze_host {
bool registerNatives_tts(std::string* error);
bool registerTtsNatives(std::string* error) {
    return registerNatives_tts(error);
}
}  // namespace bro::bronze_host
