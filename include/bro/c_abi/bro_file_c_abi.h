// =============================================================================
// bro_file_c_abi.h — Pure C-ABI declarations for bro.file
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, zero JSContext, direct native CPU register call.
// =============================================================================

#ifndef BRO_FILE_C_ABI_H
#define BRO_FILE_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Interface bro.file.Blob ---
void* bro_Blob_create(void* blobParts, void* options);
void  bro_Blob_destroy(void* self);
uint64_t bro_Blob_get_size(void* self);
const char* bro_Blob_get_type(void* self);
void* bro_Blob_slice(void* self, int64_t start, int64_t end, const char* contentType);
void* bro_Blob_text(void* self);
void* bro_Blob_arrayBuffer(void* self);
void* bro_Blob_bytes(void* self);

// --- Interface bro.file.File ---
void* bro_File_create(void* fileBits, const char* fileName, void* options);
void  bro_File_destroy(void* self);
const char* bro_File_get_name(void* self);
int64_t bro_File_get_lastModified(void* self);
const char* bro_File_get_webkitRelativePath(void* self);
const char* bro_File_get_path(void* self);

// --- Interface bro.file.FileReader ---
void* bro_FileReader_create(void);
void  bro_FileReader_destroy(void* self);
uint16_t bro_FileReader_get_readyState(void* self);
void* bro_FileReader_get_result(void* self);
void* bro_FileReader_get_error(void* self);
void* bro_FileReader_get_onloadstart(void* self);
void bro_FileReader_set_onloadstart(void* self, void* val);
void* bro_FileReader_get_onprogress(void* self);
void bro_FileReader_set_onprogress(void* self, void* val);
void* bro_FileReader_get_onload(void* self);
void bro_FileReader_set_onload(void* self, void* val);
void* bro_FileReader_get_onabort(void* self);
void bro_FileReader_set_onabort(void* self, void* val);
void* bro_FileReader_get_onerror(void* self);
void bro_FileReader_set_onerror(void* self, void* val);
void* bro_FileReader_get_onloadend(void* self);
void bro_FileReader_set_onloadend(void* self, void* val);
void bro_FileReader_readAsArrayBuffer(void* self, void* blob);
void bro_FileReader_readAsBinaryString(void* self, void* blob);
void bro_FileReader_readAsText(void* self, void* blob, const char* encoding);
void bro_FileReader_readAsDataURL(void* self, void* blob);
void bro_FileReader_abort(void* self);
void bro_FileReader_addEventListener(void* self, const char* type, void* listener);
void bro_FileReader_removeEventListener(void* self, const char* type, void* listener);

// --- Interface bro.file.URLSearchParams ---
void* bro_URLSearchParams_create(void* init);
void  bro_URLSearchParams_destroy(void* self);
void bro_URLSearchParams_append(void* self, const char* name, const char* value);
void bro_URLSearchParams_delete(void* self, const char* name);
const char* bro_URLSearchParams_get(void* self, const char* name);
void* bro_URLSearchParams_getAll(void* self, const char* name);
bool bro_URLSearchParams_has(void* self, const char* name);
void bro_URLSearchParams_set(void* self, const char* name, const char* value);
const char* bro_URLSearchParams_toString(void* self);

// --- Interface bro.file.URL ---
void* bro_URL_create(const char* url, const char* base);
void  bro_URL_destroy(void* self);
const char* bro_URL_get_href(void* self);
void bro_URL_set_href(void* self, const char* val);
const char* bro_URL_get_origin(void* self);
const char* bro_URL_get_protocol(void* self);
void bro_URL_set_protocol(void* self, const char* val);
const char* bro_URL_get_host(void* self);
void bro_URL_set_host(void* self, const char* val);
const char* bro_URL_get_hostname(void* self);
void bro_URL_set_hostname(void* self, const char* val);
const char* bro_URL_get_port(void* self);
void bro_URL_set_port(void* self, const char* val);
const char* bro_URL_get_pathname(void* self);
void bro_URL_set_pathname(void* self, const char* val);
const char* bro_URL_get_search(void* self);
void bro_URL_set_search(void* self, const char* val);
void* bro_URL_get_searchParams(void* self);
const char* bro_URL_get_hash(void* self);
void bro_URL_set_hash(void* self, const char* val);
const char* bro_URL_toJSON(void* self);
const char* bro_URL_toString(void* self);
const char* bro_URL_createObjectURL(void* obj);
void bro_URL_revokeObjectURL(const char* url);
void* bro_URL_parse(const char* url, const char* base);

#ifdef __cplusplus
}
#endif

#endif // BRO_FILE_C_ABI_H
