// =============================================================================
// bro_file_c_abi.cpp — C++ forwarding implementations for bro.file
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_file_c_abi.h"
#include <cstdint>
#include <string>
#include <vector>

struct BroBlobImpl {
    std::vector<uint8_t> bytes;
    std::string type;
    uint64_t size = 0;
};

struct BroFileImpl {
    BroBlobImpl blob;
    std::string name;
    int64_t lastModified = 0;
    std::string webkitRelativePath;
    std::string path;
};

struct BroFileReaderImpl {
    uint16_t readyState = 0;
    void* result = nullptr;
    void* error = nullptr;
    void* onloadstart = nullptr;
    void* onprogress = nullptr;
    void* onload = nullptr;
    void* onabort = nullptr;
    void* onerror = nullptr;
    void* onloadend = nullptr;
};

struct BroURLSearchParamsImpl {
    std::string query;
};

struct BroURLImpl {
    std::string href;
    std::string origin;
    std::string protocol;
    std::string host;
    std::string hostname;
    std::string port;
    std::string pathname;
    std::string search;
    std::string hash;
};

extern "C" {

// --- Interface bro.file.Blob ---
void* bro_Blob_create(void* /*blobParts*/, void* /*options*/) {
    return new BroBlobImpl();
}

void bro_Blob_destroy(void* self) {
    delete static_cast<BroBlobImpl*>(self);
}

uint64_t bro_Blob_get_size(void* self) {
    return self ? static_cast<BroBlobImpl*>(self)->size : 0;
}

const char* bro_Blob_get_type(void* self) {
    return self ? static_cast<BroBlobImpl*>(self)->type.c_str() : "";
}

void* bro_Blob_slice(void* /*self*/, int64_t /*start*/, int64_t /*end*/, const char* /*contentType*/) {
    return new BroBlobImpl();
}

void* bro_Blob_text(void* /*self*/) {
    return nullptr;
}

void* bro_Blob_arrayBuffer(void* /*self*/) {
    return nullptr;
}

void* bro_Blob_bytes(void* /*self*/) {
    return nullptr;
}

// --- Interface bro.file.File ---
void* bro_File_create(void* /*fileBits*/, const char* fileName, void* /*options*/) {
    auto* f = new BroFileImpl();
    if (fileName) f->name = fileName;
    return f;
}

void bro_File_destroy(void* self) {
    delete static_cast<BroFileImpl*>(self);
}

const char* bro_File_get_name(void* self) {
    return self ? static_cast<BroFileImpl*>(self)->name.c_str() : "";
}

int64_t bro_File_get_lastModified(void* self) {
    return self ? static_cast<BroFileImpl*>(self)->lastModified : 0;
}

const char* bro_File_get_webkitRelativePath(void* self) {
    return self ? static_cast<BroFileImpl*>(self)->webkitRelativePath.c_str() : "";
}

const char* bro_File_get_path(void* self) {
    return self ? static_cast<BroFileImpl*>(self)->path.c_str() : "";
}

// --- Interface bro.file.FileReader ---
void* bro_FileReader_create(void) {
    return new BroFileReaderImpl();
}

void bro_FileReader_destroy(void* self) {
    delete static_cast<BroFileReaderImpl*>(self);
}

uint16_t bro_FileReader_get_readyState(void* self) {
    return self ? static_cast<BroFileReaderImpl*>(self)->readyState : 0;
}

void* bro_FileReader_get_result(void* self) {
    return self ? static_cast<BroFileReaderImpl*>(self)->result : nullptr;
}

void* bro_FileReader_get_error(void* self) {
    return self ? static_cast<BroFileReaderImpl*>(self)->error : nullptr;
}

void* bro_FileReader_get_onloadstart(void* self) {
    return self ? static_cast<BroFileReaderImpl*>(self)->onloadstart : nullptr;
}

void bro_FileReader_set_onloadstart(void* self, void* val) {
    if (self) static_cast<BroFileReaderImpl*>(self)->onloadstart = val;
}

void* bro_FileReader_get_onprogress(void* self) {
    return self ? static_cast<BroFileReaderImpl*>(self)->onprogress : nullptr;
}

void bro_FileReader_set_onprogress(void* self, void* val) {
    if (self) static_cast<BroFileReaderImpl*>(self)->onprogress = val;
}

void* bro_FileReader_get_onload(void* self) {
    return self ? static_cast<BroFileReaderImpl*>(self)->onload : nullptr;
}

void bro_FileReader_set_onload(void* self, void* val) {
    if (self) static_cast<BroFileReaderImpl*>(self)->onload = val;
}

void* bro_FileReader_get_onabort(void* self) {
    return self ? static_cast<BroFileReaderImpl*>(self)->onabort : nullptr;
}

void bro_FileReader_set_onabort(void* self, void* val) {
    if (self) static_cast<BroFileReaderImpl*>(self)->onabort = val;
}

void* bro_FileReader_get_onerror(void* self) {
    return self ? static_cast<BroFileReaderImpl*>(self)->onerror : nullptr;
}

void bro_FileReader_set_onerror(void* self, void* val) {
    if (self) static_cast<BroFileReaderImpl*>(self)->onerror = val;
}

void* bro_FileReader_get_onloadend(void* self) {
    return self ? static_cast<BroFileReaderImpl*>(self)->onloadend : nullptr;
}

void bro_FileReader_set_onloadend(void* self, void* val) {
    if (self) static_cast<BroFileReaderImpl*>(self)->onloadend = val;
}

void bro_FileReader_readAsArrayBuffer(void* /*self*/, void* /*blob*/) {}
void bro_FileReader_readAsBinaryString(void* /*self*/, void* /*blob*/) {}
void bro_FileReader_readAsText(void* /*self*/, void* /*blob*/, const char* /*encoding*/) {}
void bro_FileReader_readAsDataURL(void* /*self*/, void* /*blob*/) {}
void bro_FileReader_abort(void* /*self*/) {}
void bro_FileReader_addEventListener(void* /*self*/, const char* /*type*/, void* /*listener*/) {}
void bro_FileReader_removeEventListener(void* /*self*/, const char* /*type*/, void* /*listener*/) {}

// --- Interface bro.file.URLSearchParams ---
void* bro_URLSearchParams_create(void* /*init*/) {
    return new BroURLSearchParamsImpl();
}

void bro_URLSearchParams_destroy(void* self) {
    delete static_cast<BroURLSearchParamsImpl*>(self);
}

void bro_URLSearchParams_append(void* /*self*/, const char* /*name*/, const char* /*value*/) {}
void bro_URLSearchParams_delete(void* /*self*/, const char* /*name*/) {}

const char* bro_URLSearchParams_get(void* /*self*/, const char* /*name*/) {
    return "";
}

void* bro_URLSearchParams_getAll(void* /*self*/, const char* /*name*/) {
    return nullptr;
}

bool bro_URLSearchParams_has(void* /*self*/, const char* /*name*/) {
    return false;
}

void bro_URLSearchParams_set(void* /*self*/, const char* /*name*/, const char* /*value*/) {}

const char* bro_URLSearchParams_toString(void* /*self*/) {
    return "";
}

// --- Interface bro.file.URL ---
void* bro_URL_create(const char* url, const char* /*base*/) {
    auto* u = new BroURLImpl();
    if (url) u->href = url;
    return u;
}

void bro_URL_destroy(void* self) {
    delete static_cast<BroURLImpl*>(self);
}

const char* bro_URL_get_href(void* self) {
    return self ? static_cast<BroURLImpl*>(self)->href.c_str() : "";
}

void bro_URL_set_href(void* self, const char* val) {
    if (self && val) static_cast<BroURLImpl*>(self)->href = val;
}

const char* bro_URL_get_origin(void* self) {
    return self ? static_cast<BroURLImpl*>(self)->origin.c_str() : "";
}

const char* bro_URL_get_protocol(void* self) {
    return self ? static_cast<BroURLImpl*>(self)->protocol.c_str() : "";
}

void bro_URL_set_protocol(void* self, const char* val) {
    if (self && val) static_cast<BroURLImpl*>(self)->protocol = val;
}

const char* bro_URL_get_host(void* self) {
    return self ? static_cast<BroURLImpl*>(self)->host.c_str() : "";
}

void bro_URL_set_host(void* self, const char* val) {
    if (self && val) static_cast<BroURLImpl*>(self)->host = val;
}

const char* bro_URL_get_hostname(void* self) {
    return self ? static_cast<BroURLImpl*>(self)->hostname.c_str() : "";
}

void bro_URL_set_hostname(void* self, const char* val) {
    if (self && val) static_cast<BroURLImpl*>(self)->hostname = val;
}

const char* bro_URL_get_port(void* self) {
    return self ? static_cast<BroURLImpl*>(self)->port.c_str() : "";
}

void bro_URL_set_port(void* self, const char* val) {
    if (self && val) static_cast<BroURLImpl*>(self)->port = val;
}

const char* bro_URL_get_pathname(void* self) {
    return self ? static_cast<BroURLImpl*>(self)->pathname.c_str() : "";
}

void bro_URL_set_pathname(void* self, const char* val) {
    if (self && val) static_cast<BroURLImpl*>(self)->pathname = val;
}

const char* bro_URL_get_search(void* self) {
    return self ? static_cast<BroURLImpl*>(self)->search.c_str() : "";
}

void bro_URL_set_search(void* self, const char* val) {
    if (self && val) static_cast<BroURLImpl*>(self)->search = val;
}

void* bro_URL_get_searchParams(void* /*self*/) {
    return nullptr;
}

const char* bro_URL_get_hash(void* self) {
    return self ? static_cast<BroURLImpl*>(self)->hash.c_str() : "";
}

void bro_URL_set_hash(void* self, const char* val) {
    if (self && val) static_cast<BroURLImpl*>(self)->hash = val;
}

const char* bro_URL_toJSON(void* self) {
    return self ? static_cast<BroURLImpl*>(self)->href.c_str() : "";
}

const char* bro_URL_toString(void* self) {
    return self ? static_cast<BroURLImpl*>(self)->href.c_str() : "";
}

const char* bro_URL_createObjectURL(void* /*obj*/) {
    return "blob:bro/cabi";
}

void bro_URL_revokeObjectURL(const char* /*url*/) {}

void* bro_URL_parse(const char* url, const char* /*base*/) {
    if (!url) return nullptr;
    auto* u = new BroURLImpl();
    u->href = url;
    return u;
}

} // extern "C"
