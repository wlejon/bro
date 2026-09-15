// native_stt.cpp — Bronze native entry points for bro.stt
#include "natives/stt/native_stt_decl.h"

#include <cstdint>
#include <string>

namespace {

struct BroWhisperTokenizerImpl { bool loaded = true; };
struct BroWhisperModelImpl { bool loaded = true; std::string device = "CPU"; };
struct BroWhisperSessionImpl { bool loaded = true; };
struct BroParakeetTokenizerImpl { bool loaded = true; };
struct BroParakeetModelImpl { bool loaded = true; std::string device = "CPU"; };
struct BroParakeetSessionImpl { bool loaded = true; };
struct BroQwenAsrModelImpl { bool loaded = true; std::string device = "CPU"; };
struct BroQwenAsrSessionImpl { bool loaded = true; };
struct BroQwenAsrStreamImpl { bool loaded = true; };

}  // namespace

extern "C" {

// WhisperTokenizer
void bro_stt_WhisperTokenizer_dtor(void* self) {
    delete static_cast<BroWhisperTokenizerImpl*>(self);
}
void* bro_stt_WhisperTokenizer_ctor(void) {
    return new BroWhisperTokenizerImpl();
}
bool bro_stt_WhisperTokenizer_loaded_get(void* self) {
    return self && static_cast<BroWhisperTokenizerImpl*>(self)->loaded;
}

// WhisperModel
void bro_stt_WhisperModel_dtor(void* self) {
    delete static_cast<BroWhisperModelImpl*>(self);
}
void* bro_stt_WhisperModel_ctor(void) {
    return new BroWhisperModelImpl();
}
bool bro_stt_WhisperModel_loaded_get(void* self) {
    return self && static_cast<BroWhisperModelImpl*>(self)->loaded;
}
const char* bro_stt_WhisperModel_device_get(void* self) {
    return self ? static_cast<BroWhisperModelImpl*>(self)->device.c_str() : "CPU";
}
void* bro_stt_WhisperModel_createSession(void* /*self*/) {
    return new BroWhisperSessionImpl();
}

// WhisperSession
void bro_stt_WhisperSession_dtor(void* self) {
    delete static_cast<BroWhisperSessionImpl*>(self);
}
void* bro_stt_WhisperSession_ctor(void) {
    return new BroWhisperSessionImpl();
}
bool bro_stt_WhisperSession_loaded_get(void* self) {
    return self && static_cast<BroWhisperSessionImpl*>(self)->loaded;
}
void bro_stt_WhisperSession_reset(void* /*self*/) {
}

// ParakeetTokenizer
void bro_stt_ParakeetTokenizer_dtor(void* self) {
    delete static_cast<BroParakeetTokenizerImpl*>(self);
}
void* bro_stt_ParakeetTokenizer_ctor(void) {
    return new BroParakeetTokenizerImpl();
}
bool bro_stt_ParakeetTokenizer_loaded_get(void* self) {
    return self && static_cast<BroParakeetTokenizerImpl*>(self)->loaded;
}

// ParakeetModel
void bro_stt_ParakeetModel_dtor(void* self) {
    delete static_cast<BroParakeetModelImpl*>(self);
}
void* bro_stt_ParakeetModel_ctor(void) {
    return new BroParakeetModelImpl();
}
bool bro_stt_ParakeetModel_loaded_get(void* self) {
    return self && static_cast<BroParakeetModelImpl*>(self)->loaded;
}
const char* bro_stt_ParakeetModel_device_get(void* self) {
    return self ? static_cast<BroParakeetModelImpl*>(self)->device.c_str() : "CPU";
}
void* bro_stt_ParakeetModel_createSession(void* /*self*/) {
    return new BroParakeetSessionImpl();
}

// ParakeetSession
void bro_stt_ParakeetSession_dtor(void* self) {
    delete static_cast<BroParakeetSessionImpl*>(self);
}
void* bro_stt_ParakeetSession_ctor(void) {
    return new BroParakeetSessionImpl();
}
bool bro_stt_ParakeetSession_loaded_get(void* self) {
    return self && static_cast<BroParakeetSessionImpl*>(self)->loaded;
}
void bro_stt_ParakeetSession_reset(void* /*self*/) {
}

// QwenAsrModel
void bro_stt_QwenAsrModel_dtor(void* self) {
    delete static_cast<BroQwenAsrModelImpl*>(self);
}
void* bro_stt_QwenAsrModel_ctor(void) {
    return new BroQwenAsrModelImpl();
}
bool bro_stt_QwenAsrModel_loaded_get(void* self) {
    return self && static_cast<BroQwenAsrModelImpl*>(self)->loaded;
}
const char* bro_stt_QwenAsrModel_device_get(void* self) {
    return self ? static_cast<BroQwenAsrModelImpl*>(self)->device.c_str() : "CPU";
}
void* bro_stt_QwenAsrModel_createSession(void* /*self*/) {
    return new BroQwenAsrSessionImpl();
}

// QwenAsrSession
void bro_stt_QwenAsrSession_dtor(void* self) {
    delete static_cast<BroQwenAsrSessionImpl*>(self);
}
void* bro_stt_QwenAsrSession_ctor(void) {
    return new BroQwenAsrSessionImpl();
}
bool bro_stt_QwenAsrSession_loaded_get(void* self) {
    return self && static_cast<BroQwenAsrSessionImpl*>(self)->loaded;
}
void bro_stt_QwenAsrSession_reset(void* /*self*/) {
}

// QwenAsrStream
void bro_stt_QwenAsrStream_dtor(void* self) {
    delete static_cast<BroQwenAsrStreamImpl*>(self);
}
void* bro_stt_QwenAsrStream_ctor(void) {
    return new BroQwenAsrStreamImpl();
}
bool bro_stt_QwenAsrStream_loaded_get(void* self) {
    return self && static_cast<BroQwenAsrStreamImpl*>(self)->loaded;
}

// Namespace
void bro_stt_init(void) {
}

}  // extern "C"

namespace bro::bronze_host {
bool registerNatives_stt(std::string* error);
bool registerSttNatives(std::string* error) {
    return registerNatives_stt(error);
}
}  // namespace bro::bronze_host
